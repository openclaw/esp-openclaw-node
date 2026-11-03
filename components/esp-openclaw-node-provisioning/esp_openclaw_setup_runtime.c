#include "esp_openclaw_setup_runtime.h"
#include "esp_openclaw_setup_native.h"

#if OPENCLAW_SETUP_NATIVE && !CONFIG_BT_CTRL_MODEM_SLEEP
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_random.h"
#include "esp_srp.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "protocomm_ble.h"
#include "esp_openclaw_setup_mailbox.h"
#include "esp_openclaw_setup_security2.h"

typedef enum { LOCAL_OPEN, LOCAL_NEUTRAL, LOCAL_CONFIRM, LOCAL_ACTIVATE, LOCAL_DRAIN } command_kind_t;
typedef struct { command_kind_t kind; uint64_t request, generation, peer; } command_t;
struct esp_openclaw_setup_runtime {
    portMUX_TYPE lock;
    QueueHandle_t commands;
    void (*gateway_reconnect)(void *context);
    void *context;
    char label[65], model[65];
    uint64_t issued, request, generation, peer;
    bool cancelled, ble_started, stop_failed;
    esp_err_t error;
    esp_openclaw_setup_runtime_snapshot_t view;
    esp_openclaw_setup_session_t session;
    esp_openclaw_setup_wifi_t wifi;
    esp_openclaw_setup_gateway_t gateway;
    esp_openclaw_setup_transfer_t transfer;
    esp_openclaw_setup_dispatcher_t dispatcher;
    esp_openclaw_setup_mailbox_t mailbox;
    protocomm_t *pc;
    protocomm_security2_params_t parameters;
    char *salt, *verifier;
    int verifier_size;
    char pop[33], service[30];
    uint8_t challenge[32];
};
static esp_openclaw_setup_runtime_handle_t singleton; /* Boot owner creates once. */

static void wipe(void *data, size_t size)
{ volatile uint8_t *bytes = data; while (size--) *bytes++ = 0; }

static uint64_t now_ms(void *context)
{ (void)context; return (uint64_t)esp_timer_get_time() / 1000; }

static bool cancelled(esp_openclaw_setup_runtime_handle_t runtime)
{
    portENTER_CRITICAL(&runtime->lock);
    bool value = runtime->cancelled;
    portEXIT_CRITICAL(&runtime->lock);
    return value;
}

static bool authenticated(uint64_t peer, void *context)
{ return !cancelled(context) && esp_openclaw_setup_security2_peer_is_current(peer); }

static void cancel_operation(esp_openclaw_setup_token_t token, void *context)
{ esp_openclaw_setup_dispatcher_cancel(&((esp_openclaw_setup_runtime_handle_t)context)->dispatcher, token); }

static bool hello(uint64_t generation, uint64_t peer, esp_openclaw_setup_bytes_t *label,
    esp_openclaw_setup_bytes_t *model, const uint8_t **challenge, void *context)
{
    esp_openclaw_setup_runtime_handle_t runtime = context;
    if (generation != runtime->generation || peer != runtime->peer || !authenticated(peer, runtime)) return false;
    *label = (esp_openclaw_setup_bytes_t){(const uint8_t *)runtime->label, strlen(runtime->label)};
    *model = (esp_openclaw_setup_bytes_t){(const uint8_t *)runtime->model, strlen(runtime->model)};
    *challenge = runtime->challenge;
    return true;
}

static bool current(esp_openclaw_setup_runtime_handle_t runtime)
{
    esp_openclaw_setup_snapshot_t snapshot = esp_openclaw_setup_snapshot(&runtime->session);
    return !cancelled(runtime) && snapshot.generation == runtime->generation &&
        snapshot.phase != ESP_OPENCLAW_SETUP_CLOSED && snapshot.phase != ESP_OPENCLAW_SETUP_CLOSING;
}

static void close_server(esp_openclaw_setup_runtime_handle_t runtime, bool retry)
{
    esp_openclaw_setup_security2_revoke();
    esp_openclaw_setup_close(&runtime->session);
    esp_openclaw_setup_mailbox_close(&runtime->mailbox, runtime->generation, runtime->peer);
    esp_openclaw_setup_transfer_close(&runtime->transfer);
    wipe(runtime->pop, sizeof(runtime->pop));
    wipe(runtime->challenge, sizeof(runtime->challenge));
    if (runtime->stop_failed && !retry) return;
    if (runtime->ble_started) {
        esp_err_t error = protocomm_ble_stop(runtime->pc); /* Owner task, no locks: drains host callbacks. */
        if (error != ESP_OK) { runtime->error = error; runtime->stop_failed = true; return; }
        runtime->ble_started = false;
    }
    runtime->stop_failed = false;
    if (runtime->pc) { protocomm_delete(runtime->pc); runtime->pc = NULL; }
    if (runtime->salt) { wipe(runtime->salt, 16); free(runtime->salt); runtime->salt = NULL; }
    if (runtime->verifier) {
        wipe(runtime->verifier, runtime->verifier_size); free(runtime->verifier); runtime->verifier = NULL;
    }
    runtime->parameters = (protocomm_security2_params_t){0};
    wipe(runtime->service, sizeof(runtime->service));
}

static void open_server(esp_openclaw_setup_runtime_handle_t runtime)
{
    uint8_t seed[32] = {0};
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (cancelled(runtime)) return;
    if (!esp_openclaw_setup_security2_quiet_logs()) goto finished;
    if (esp_openclaw_setup_open(&runtime->session, &runtime->generation) != ESP_OPENCLAW_SETUP_OK) goto finished;
    error = esp_openclaw_node_wifi_setup_seed(seed);
    if (error != ESP_OK) goto finished;
    for (unsigned i = 0; i < 16; ++i) snprintf(runtime->pop + i * 2, 3, "%02x", seed[i]);
    snprintf(runtime->service, sizeof(runtime->service), "PROV_OC_%02x%02x%02x%02x", seed[16], seed[17], seed[18], seed[19]);
    wipe(seed, sizeof(seed));
    error = esp_srp_gen_salt_verifier("openclaw", 8, runtime->pop, 32,
        &runtime->salt, 16, &runtime->verifier, &runtime->verifier_size);
    if (error != ESP_OK || !current(runtime)) goto finished;
    if (runtime->verifier_size <= 0 || runtime->verifier_size > UINT16_MAX) { error = ESP_ERR_INVALID_SIZE; goto finished; }
    runtime->parameters = (protocomm_security2_params_t){runtime->salt, 16, runtime->verifier, runtime->verifier_size};
    runtime->pc = protocomm_new();
    if (!runtime->pc) { error = ESP_ERR_NO_MEM; goto finished; }
    error = protocomm_set_security(runtime->pc, "prov-session", esp_openclaw_setup_security2(), &runtime->parameters);
    if (error != ESP_OK) goto finished;
    error = protocomm_set_version(runtime->pc, "proto-ver",
        "{\"prov\":{\"ver\":\"v1.1\",\"sec_ver\":2,\"sec_patch_ver\":1,\"cap\":[]}}");
    if (error != ESP_OK) goto finished;
    error = protocomm_add_endpoint(runtime->pc, "oc-setup", esp_openclaw_setup_mailbox_endpoint, &runtime->mailbox);
    if (error != ESP_OK) goto finished;
    esp_openclaw_setup_snapshot_t session = esp_openclaw_setup_snapshot(&runtime->session);
    if (!current(runtime) || !esp_openclaw_setup_mailbox_open(&runtime->mailbox,
            runtime->generation, 0, session.opened_ms)) { error = ESP_ERR_INVALID_STATE; goto finished; }
    protocomm_ble_name_uuid_t endpoints[] = {{"prov-session", 0xff51}, {"proto-ver", 0xff53}, {"oc-setup", 0xff54}};
    protocomm_ble_config_t ble = {.service_uuid = {0x07, 0xed, 0x9b, 0x2d, 0x0f, 0x06, 0x7c, 0x87,
        0x9b, 0x43, 0x43, 0x6b, 0x4d, 0x24, 0x75, 0x17}, .nu_lookup_count = 3, .nu_lookup = endpoints};
    snprintf(ble.device_name, sizeof(ble.device_name), "%s", runtime->service);
    if (!current(runtime)) { error = ESP_ERR_INVALID_STATE; goto finished; }
    error = protocomm_ble_start(runtime->pc, &ble);
    runtime->ble_started = error == ESP_OK;
finished:
    wipe(seed, sizeof(seed));
    if (error != ESP_OK) runtime->error = error;
    if (error != ESP_OK || !current(runtime)) close_server(runtime, false);
}

static void publish(esp_openclaw_setup_runtime_handle_t runtime)
{
    esp_openclaw_setup_runtime_snapshot_t view = {.request_id = runtime->request, .peer = runtime->peer,
        .error = runtime->error, .session = esp_openclaw_setup_snapshot(&runtime->session),
        .wifi = esp_openclaw_setup_wifi_snapshot(&runtime->wifi),
        .gateway = esp_openclaw_setup_gateway_snapshot(&runtime->gateway)};
    bool closing = view.session.phase == ESP_OPENCLAW_SETUP_CLOSED || view.session.phase == ESP_OPENCLAW_SETUP_CLOSING;
    view.can_open = closing && view.session.phase == ESP_OPENCLAW_SETUP_CLOSED && !runtime->pc &&
        !view.wifi.lease_held && !view.gateway.lease_held;
    view.phase = view.can_open ? ESP_OPENCLAW_SETUP_RUNTIME_CLOSED : closing ?
        ESP_OPENCLAW_SETUP_RUNTIME_CLOSING : ESP_OPENCLAW_SETUP_RUNTIME_ACTIVE;
    if (runtime->error != ESP_OK && closing) view.phase = ESP_OPENCLAW_SETUP_RUNTIME_ERROR;
    if (!closing && !cancelled(runtime)) {
        if (view.session.phase == ESP_OPENCLAW_SETUP_DISCOVERABLE) {
            snprintf(view.service, sizeof(view.service), "%s", runtime->service);
            snprintf(view.pop, sizeof(view.pop), "%s", runtime->pop);
            snprintf(view.qr, sizeof(view.qr), "{\"ver\":\"v1\",\"name\":\"%s\",\"username\":\"openclaw\",\"pop\":\"%s\",\"transport\":\"ble\"}",
                runtime->service, runtime->pop);
        } else snprintf(view.comparison, sizeof(view.comparison), "%02X%02X %02X%02X",
            runtime->challenge[0], runtime->challenge[1], runtime->challenge[2], runtime->challenge[3]);
    }
    portENTER_CRITICAL(&runtime->lock);
    if (runtime->request == runtime->issued) {
        if (runtime->cancelled) { wipe(view.pop, sizeof(view.pop)); wipe(view.qr, sizeof(view.qr)); wipe(view.comparison, sizeof(view.comparison)); }
        runtime->view = view;
    }
    portEXIT_CRITICAL(&runtime->lock);
    wipe(&view, sizeof(view));
}

static void command(esp_openclaw_setup_runtime_handle_t runtime, command_t cmd)
{
    portENTER_CRITICAL(&runtime->lock);
    bool matching_request = cmd.request == runtime->issued;
    portEXIT_CRITICAL(&runtime->lock);
    if (!matching_request) return;
    if (cmd.kind == LOCAL_OPEN) {
        runtime->request = cmd.request; runtime->error = ESP_OK; runtime->peer = 0;
        open_server(runtime);
    } else if (cmd.request == runtime->request && cmd.generation == runtime->generation && cmd.peer == runtime->peer) {
        if (cmd.kind == LOCAL_DRAIN && !current(runtime)) {
            runtime->error = ESP_OK;
            close_server(runtime, true);
            esp_openclaw_setup_wifi_snapshot_t wifi = esp_openclaw_setup_wifi_snapshot(&runtime->wifi);
            if (wifi.operation_pending && wifi.attempt.state == ESP_OPENCLAW_NODE_WIFI_RECOVERY_REQUIRED) {
                esp_err_t error = esp_openclaw_setup_wifi_retry_drain(&runtime->wifi);
                if (runtime->error == ESP_OK) runtime->error = error;
            }
        } else if (current(runtime) && authenticated(cmd.peer, runtime)) {
            if (cmd.kind == LOCAL_ACTIVATE && esp_openclaw_setup_snapshot(&runtime->session).phase == ESP_OPENCLAW_SETUP_READY)
                runtime->error = esp_openclaw_setup_dispatcher_activate(&runtime->dispatcher, cmd.generation);
            if (cmd.kind == LOCAL_NEUTRAL) esp_openclaw_setup_input_released(&runtime->session, cmd.generation);
            if (cmd.kind == LOCAL_CONFIRM && esp_openclaw_setup_confirm(&runtime->session, cmd.generation) == ESP_OPENCLAW_SETUP_OK)
                runtime->error = esp_openclaw_setup_dispatcher_activate(&runtime->dispatcher, cmd.generation);
        }
    }
}

static void task(void *context)
{
    esp_openclaw_setup_runtime_handle_t runtime = context;
    for (;;) {
        command_t cmd;
        portENTER_CRITICAL(&runtime->lock);
        bool idle = runtime->view.can_open || (runtime->stop_failed &&
            !runtime->view.wifi.lease_held && !runtime->view.gateway.lease_held);
        portEXIT_CRITICAL(&runtime->lock);
        if (xQueueReceive(runtime->commands, &cmd, idle ? portMAX_DELAY : pdMS_TO_TICKS(20)) == pdTRUE) command(runtime, cmd);
        if (current(runtime) && runtime->peer == 0) {
            uint64_t peer = esp_openclaw_setup_security2_peer();
            if (peer) {
                esp_fill_random(runtime->challenge, sizeof(runtime->challenge));
                esp_openclaw_setup_snapshot_t session = esp_openclaw_setup_snapshot(&runtime->session);
                if (authenticated(peer, runtime) && esp_openclaw_setup_attach(&runtime->session, runtime->generation, peer) == ESP_OPENCLAW_SETUP_OK &&
                    esp_openclaw_setup_transfer_open(&runtime->transfer, runtime->generation, peer) &&
                    esp_openclaw_setup_mailbox_open(&runtime->mailbox, runtime->generation, peer, session.opened_ms)) runtime->peer = peer;
                else { runtime->error = ESP_ERR_INVALID_STATE; close_server(runtime, false); }
            }
        }
        if (current(runtime)) esp_openclaw_setup_mailbox_poll(&runtime->mailbox);
        if (!current(runtime)) close_server(runtime, false);
        bool released = false;
        esp_err_t error = esp_openclaw_setup_dispatcher_poll(&runtime->dispatcher, &released);
        if (error != ESP_OK) runtime->error = error;
        if (released && runtime->gateway_reconnect) runtime->gateway_reconnect(runtime->context);
        publish(runtime);
    }
}

esp_err_t esp_openclaw_setup_runtime_create(const esp_openclaw_setup_runtime_config_t *config,
    esp_openclaw_setup_runtime_handle_t *output)
{
    if (!config || !output || !config->node || !config->label || !config->model ||
        !strlen(config->label) || strlen(config->label) > 64 || !strlen(config->model) || strlen(config->model) > 64) return ESP_ERR_INVALID_ARG;
    if (singleton) return ESP_ERR_INVALID_STATE;
    esp_openclaw_setup_runtime_handle_t runtime = calloc(1, sizeof(*runtime));
    if (!runtime) return ESP_ERR_NO_MEM;
    runtime->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    runtime->gateway_reconnect = config->gateway_reconnect; runtime->context = config->context;
    snprintf(runtime->label, sizeof(runtime->label), "%s", config->label);
    snprintf(runtime->model, sizeof(runtime->model), "%s", config->model);
    runtime->view.can_open = true;
    esp_openclaw_setup_callbacks_t callbacks = {now_ms, authenticated, cancel_operation, runtime};
    esp_openclaw_setup_init(&runtime->session, &callbacks);
    esp_openclaw_setup_wifi_init(&runtime->wifi, &runtime->session);
    esp_openclaw_setup_gateway_init(&runtime->gateway, &runtime->session, config->node);
    esp_openclaw_setup_transfer_init(&runtime->transfer, &runtime->session);
    esp_openclaw_setup_dispatcher_init(&runtime->dispatcher, &runtime->session, &runtime->transfer,
        &runtime->wifi, &runtime->gateway, hello, runtime);
    esp_openclaw_setup_mailbox_init(&runtime->mailbox, &runtime->transfer, &runtime->dispatcher, now_ms, runtime);
    runtime->commands = xQueueCreate(4, sizeof(command_t));
    if (!runtime->commands || xTaskCreate(task, "oc_setup", 8192, runtime, 4, NULL) != pdPASS) {
        if (runtime->commands) vQueueDelete(runtime->commands);
        free(runtime); return ESP_ERR_NO_MEM;
    }
    singleton = runtime; *output = runtime;
    return ESP_OK;
}

esp_err_t esp_openclaw_setup_runtime_open(esp_openclaw_setup_runtime_handle_t runtime, uint64_t *request)
{
    if (!runtime || !request) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&runtime->lock);
    if (!runtime->view.can_open || runtime->issued == UINT64_MAX) { portEXIT_CRITICAL(&runtime->lock); return ESP_ERR_INVALID_STATE; }
    uint64_t id = ++runtime->issued;
    runtime->cancelled = false;
    runtime->view = (esp_openclaw_setup_runtime_snapshot_t){.request_id = id, .phase = ESP_OPENCLAW_SETUP_RUNTIME_OPENING};
    portEXIT_CRITICAL(&runtime->lock);
    command_t cmd = {.kind = LOCAL_OPEN, .request = id};
    if (xQueueSend(runtime->commands, &cmd, 0) != pdTRUE) {
        portENTER_CRITICAL(&runtime->lock);
        runtime->view.phase = ESP_OPENCLAW_SETUP_RUNTIME_ERROR;
        runtime->view.error = ESP_ERR_TIMEOUT; runtime->view.can_open = true;
        portEXIT_CRITICAL(&runtime->lock);
        return ESP_ERR_TIMEOUT;
    }
    *request = id; return ESP_OK;
}

esp_err_t esp_openclaw_setup_runtime_cancel(esp_openclaw_setup_runtime_handle_t runtime, uint64_t request)
{
    if (!runtime || !request) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&runtime->lock);
    if (request != runtime->issued || runtime->view.can_open) { portEXIT_CRITICAL(&runtime->lock); return ESP_ERR_INVALID_STATE; }
    runtime->cancelled = true;
    esp_openclaw_setup_security2_revoke();
    wipe(runtime->view.pop, sizeof(runtime->view.pop)); wipe(runtime->view.qr, sizeof(runtime->view.qr));
    wipe(runtime->view.comparison, sizeof(runtime->view.comparison));
    portEXIT_CRITICAL(&runtime->lock);
    return ESP_OK;
}

esp_err_t esp_openclaw_setup_runtime_action(esp_openclaw_setup_runtime_handle_t runtime,
    uint64_t request, uint64_t generation, uint64_t peer, esp_openclaw_setup_local_action_t action)
{
    if (!runtime || action < ESP_OPENCLAW_SETUP_LOCAL_NEUTRAL || action > ESP_OPENCLAW_SETUP_LOCAL_RETRY) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&runtime->lock);
    bool match = request && request == runtime->issued && !runtime->view.can_open &&
        generation == runtime->view.session.generation && peer == runtime->view.peer;
    if (action != ESP_OPENCLAW_SETUP_LOCAL_RETRY) match = match && !runtime->cancelled && peer &&
        runtime->view.session.phase == ESP_OPENCLAW_SETUP_COMPARISON;
    else match = match && (runtime->view.error != ESP_OK ||
        (runtime->view.wifi.operation_pending && runtime->view.wifi.attempt.state == ESP_OPENCLAW_NODE_WIFI_RECOVERY_REQUIRED));
    command_kind_t retry = runtime->view.session.phase == ESP_OPENCLAW_SETUP_READY && !runtime->cancelled ? LOCAL_ACTIVATE : LOCAL_DRAIN;
    portEXIT_CRITICAL(&runtime->lock);
    if (!match) return ESP_ERR_INVALID_STATE;
    command_t cmd = {.kind = action == ESP_OPENCLAW_SETUP_LOCAL_NEUTRAL ? LOCAL_NEUTRAL :
        action == ESP_OPENCLAW_SETUP_LOCAL_CONFIRM ? LOCAL_CONFIRM : retry, .request = request, .generation = generation, .peer = peer};
    return xQueueSend(runtime->commands, &cmd, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool esp_openclaw_setup_runtime_snapshot(esp_openclaw_setup_runtime_handle_t runtime,
    esp_openclaw_setup_runtime_snapshot_t *snapshot)
{
    if (!runtime || !snapshot) return false;
    portENTER_CRITICAL(&runtime->lock); *snapshot = runtime->view; portEXIT_CRITICAL(&runtime->lock);
    if (cancelled(runtime) || !esp_openclaw_setup_time_is_current(snapshot->session.opened_ms, now_ms(runtime)) ||
        (snapshot->peer && !authenticated(snapshot->peer, runtime))) {
        wipe(snapshot->pop, sizeof(snapshot->pop)); wipe(snapshot->qr, sizeof(snapshot->qr));
        wipe(snapshot->comparison, sizeof(snapshot->comparison));
    }
    return true;
}

#else
esp_err_t esp_openclaw_setup_runtime_create(const esp_openclaw_setup_runtime_config_t *config,
    esp_openclaw_setup_runtime_handle_t *runtime)
{ (void)config; (void)runtime; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t esp_openclaw_setup_runtime_open(esp_openclaw_setup_runtime_handle_t runtime, uint64_t *request)
{ (void)runtime; (void)request; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t esp_openclaw_setup_runtime_cancel(esp_openclaw_setup_runtime_handle_t runtime, uint64_t request)
{ (void)runtime; (void)request; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t esp_openclaw_setup_runtime_action(esp_openclaw_setup_runtime_handle_t runtime,
    uint64_t request, uint64_t generation, uint64_t peer, esp_openclaw_setup_local_action_t action)
{ (void)runtime; (void)request; (void)generation; (void)peer; (void)action; return ESP_ERR_NOT_SUPPORTED; }
bool esp_openclaw_setup_runtime_snapshot(esp_openclaw_setup_runtime_handle_t runtime, esp_openclaw_setup_runtime_snapshot_t *snapshot)
{ (void)runtime; (void)snapshot; return false; }
#endif
