/* Deterministic boundary implementations. No room/Talk decision logic here. */
#include "room_host_fakes.h"
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_openclaw_talk.h"
#include "esp_peer_default.h"
#include "room_board.h"
#include "room_canvas.h"
#include "room_canvas_node_cmd.h"
#include "room_device_commands.h"
#include "room_face.h"
#include "room_files.h"
#include "room_media.h"

room_host_observations_t host = {.ambient = true};
struct host_mutex { bool held; bool binary; bool available; };
struct host_queue { unsigned capacity, size, count; unsigned char *data; };
struct host_timer { bool active; void *id; void (*callback)(TimerHandle_t); };
struct esp_timer { bool active; esp_timer_create_args_t args; };
struct host_task {
    const char *name;
    TaskFunction_t fn;
    void *arg;
    unsigned notifications;
};
struct esp_openclaw_node {
    esp_openclaw_node_config_t config;
    esp_openclaw_node_command_t commands[2];
    unsigned command_count;
};
static struct host_task tasks[8];
static size_t task_count;
static struct host_task *running;
static jmp_buf task_blocked;
static SemaphoreHandle_t mutex;
static QueueHandle_t queue;
static TimerHandle_t timeout;
static esp_timer_handle_t operator_timer;
static esp_openclaw_node_handle_t nodes[4];
static size_t node_count;
static unsigned mutex_depth;

typedef struct {
    esp_openclaw_node_gateway_request_cb_t callback;
    void *ctx;
    esp_openclaw_node_handle_t node;
} pending_rpc_t;
static pending_rpc_t config_rpc, create_rpc;

typedef struct {
    esp_webrtc_cfg_t config;
    esp_peer_signaling_handle_t signaling;
    esp_webrtc_event_handler_t handler;
    void *handler_ctx;
    bool ready;
} fake_rtc_t;
static fake_rtc_t *recycled_rtc;

void host_require(bool condition, const char *message)
{
    if (condition) return;
    fprintf(stderr, "HARNESS/SETUP FAILURE: %s\n", message);
    abort();
}

void host_log(const char *tag, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    fprintf(stderr, "[%s] ", tag);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
}

const char *esp_err_to_name(esp_err_t code)
{
    (void)code;
    return "synthetic host error";
}

void host_enter_critical(portMUX_TYPE *lock)
{
    host_require(*lock == 0, "recursive critical section");
    ++*lock;
    ++host.critical_depth;
}

void host_exit_critical(portMUX_TYPE *lock)
{
    host_require(*lock == 1 && host.critical_depth != 0, "unbalanced critical section");
    --*lock;
    --host.critical_depth;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    host_require(mutex == NULL, "fixture creates one room mutex");
    mutex = calloc(1, sizeof(*mutex));
    host_require(mutex != NULL, "mutex allocation");
    return mutex;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t value, TickType_t wait)
{
    (void)wait;
    if (value->binary) {
        host_require(value->available, "single-thread room fixture cannot block on callback drain");
        value->available = false;
        return pdTRUE;
    }
    host_require(value != NULL && !value->held, "room mutex already held");
    value->held = true;
    ++mutex_depth;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t value)
{
    if (value->binary) { value->available = true; return pdTRUE; }
    host_require(value != NULL && value->held, "room mutex not held");
    value->held = false;
    --mutex_depth;
    return pdTRUE;
}

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    SemaphoreHandle_t value = calloc(1, sizeof(*value));
    host_require(value != NULL, "binary semaphore allocation");
    value->binary = true;
    return value;
}
void vSemaphoreDelete(SemaphoreHandle_t value) { free(value); }

QueueHandle_t xQueueCreate(UBaseType_t capacity, UBaseType_t size)
{
    host_require(queue == NULL, "fixture creates one teardown queue");
    queue = calloc(1, sizeof(*queue));
    host_require(queue != NULL, "queue allocation");
    queue->capacity = capacity;
    queue->size = size;
    queue->data = calloc(capacity, size);
    host_require(queue->data != NULL, "queue items allocation");
    return queue;
}

BaseType_t xQueueSend(QueueHandle_t value, const void *item, TickType_t wait)
{
    host_require(wait == 0, "teardown enqueue must be nonblocking");
    if (value->count == value->capacity) return pdFALSE;
    memcpy(value->data + value->count++ * value->size, item, value->size);
    return pdTRUE;
}

BaseType_t xQueueOverwrite(QueueHandle_t value, const void *item)
{
    host_require(value->capacity == 1, "coalesced wakeup requires one slot");
    memcpy(value->data, item, value->size);
    value->count = 1;
    return pdTRUE;
}

static void block_task(void)
{
    host_require(running != NULL && mutex_depth == 0 && host.critical_depth == 0,
        "task must yield at an unlocked scheduler boundary");
    longjmp(task_blocked, 1);
}

BaseType_t xQueueReceive(QueueHandle_t value, void *item, TickType_t wait)
{
    if (value->count == 0) {
        host_require(wait == portMAX_DELAY, "expected blocking queue receive");
        block_task();
    }
    memcpy(item, value->data, value->size);
    --value->count;
    memmove(value->data, value->data + value->size, value->count * value->size);
    return pdTRUE;
}

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack,
    void *arg, UBaseType_t priority, TaskHandle_t *out)
{
    (void)stack; (void)priority;
    host_require(task_count < sizeof(tasks) / sizeof(tasks[0]), "task capacity");
    struct host_task *task = &tasks[task_count++];
    *task = (struct host_task){.fn = fn, .name = name, .arg = arg};
    if (out != NULL) *out = task;
    return pdPASS;
}

BaseType_t xTaskCreateWithCaps(TaskFunction_t fn, const char *name, uint32_t stack,
    void *arg, UBaseType_t priority, TaskHandle_t *out, UBaseType_t caps)
{
    (void)caps;
    return xTaskCreate(fn, name, stack, arg, priority, out);
}

BaseType_t xTaskNotifyGive(TaskHandle_t task)
{
    host_require(task != NULL, "notify registered worker");
    ++task->notifications;
    return pdPASS;
}

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait)
{
    host_require(running != NULL && clear == pdTRUE && wait == portMAX_DELAY,
        "start worker notification contract");
    if (running->notifications == 0) block_task();
    unsigned count = running->notifications;
    running->notifications = 0;
    return count;
}

void vTaskDelete(TaskHandle_t task)
{
    host_require(task == NULL, "only self-delete is supported");
    block_task();
}

void vTaskDelay(TickType_t wait)
{
    (void)wait;
    host_require(false, "reconnect delays must not be run implicitly");
}

void host_run_task(const char *name)
{
    host_require(running == NULL, "scheduler is non-reentrant");
    for (size_t i = 0; i < task_count; ++i) {
        if (strcmp(tasks[i].name, name) != 0) continue;
        running = &tasks[i];
        if (setjmp(task_blocked) == 0) running->fn(running->arg);
        running = NULL;
        return;
    }
    host_require(false, "requested task was never registered");
}

TimerHandle_t xTimerCreate(const char *name, TickType_t period, BaseType_t reload,
    void *id, void (*callback)(TimerHandle_t))
{
    (void)name; (void)period; (void)reload;
    host_require(timeout == NULL, "fixture creates one call timer");
    timeout = calloc(1, sizeof(*timeout));
    host_require(timeout != NULL, "timer allocation");
    timeout->callback = callback;
    timeout->id = id;
    return timeout;
}
BaseType_t xTimerStart(TimerHandle_t timer, TickType_t wait)
{ (void)wait; if (host.fail_timer) return pdFALSE; timer->active = true; return pdPASS; }
BaseType_t xTimerStop(TimerHandle_t timer, TickType_t wait)
{ (void)wait; timer->active = false; return pdPASS; }
bool host_timer_active(TimerHandle_t timer) { return timer != NULL && timer->active; }
BaseType_t xTimerDelete(TimerHandle_t timer, TickType_t wait)
{ (void)wait; host_require(timer == timeout, "delete owned timer"); free(timer); timeout = NULL; return pdPASS; }
void *pvTimerGetTimerID(TimerHandle_t timer) { return timer->id; }
void host_fire_timeout(TimerHandle_t timer) { timer->callback(timer); }

esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out)
{
    host_require(operator_timer == NULL, "one operator retry timer");
    operator_timer = calloc(1, sizeof(*operator_timer));
    host_require(operator_timer != NULL, "operator timer allocation");
    operator_timer->args = *args;
    *out = operator_timer;
    return ESP_OK;
}
esp_err_t esp_timer_stop(esp_timer_handle_t timer)
{ timer->active = false; return ESP_OK; }
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t delay)
{ (void)delay; timer->active = true; return ESP_OK; }
void host_fire_operator_timer(esp_timer_handle_t timer)
{
    host_require(timer != NULL && timer->active, "operator retry was armed");
    timer->active = false;
    timer->args.callback(timer->args.arg);
}

void esp_openclaw_node_config_init_default(esp_openclaw_node_config_t *config)
{ *config = (esp_openclaw_node_config_t){.role = "node"}; }
esp_err_t esp_openclaw_node_create(const esp_openclaw_node_config_t *config,
    esp_openclaw_node_handle_t *out)
{
    host_require(node_count < sizeof(nodes) / sizeof(nodes[0]), "node capacity");
    *out = calloc(1, sizeof(**out));
    host_require(*out != NULL, "node allocation");
    (*out)->config = *config;
    nodes[node_count++] = *out;
    return ESP_OK;
}
esp_err_t esp_openclaw_node_destroy(esp_openclaw_node_handle_t node)
{ (void)node; host_require(false, "unexpected Node destruction"); return ESP_FAIL; }
esp_err_t esp_openclaw_node_register_scope(esp_openclaw_node_handle_t node, const char *scope)
{ (void)node; (void)scope; return ESP_OK; }
esp_err_t esp_openclaw_node_register_capability(esp_openclaw_node_handle_t node, const char *capability)
{ (void)node; (void)capability; return ESP_OK; }
esp_err_t esp_openclaw_node_register_command(esp_openclaw_node_handle_t node,
    const esp_openclaw_node_command_t *command)
{
    host_require(node->command_count < 2, "only real Talk commands are registered");
    node->commands[node->command_count++] = *command;
    return ESP_OK;
}
esp_err_t esp_openclaw_node_request_connect(esp_openclaw_node_handle_t node,
    const esp_openclaw_node_connect_request_t *request)
{
    (void)node;
    host_require(request->source == ESP_OPENCLAW_NODE_CONNECT_SOURCE_SAVED_SESSION,
        "synthetic saved-session connect only; no credentials accessed");
    return ESP_OK;
}
esp_err_t esp_openclaw_node_request_disconnect(esp_openclaw_node_handle_t node)
{ (void)node; return ESP_OK; }
char *esp_openclaw_node_dup_gateway_uri(esp_openclaw_node_handle_t node)
{ return strdup(host.node_uri != NULL && strcmp(node->config.role, "node") == 0
    ? host.node_uri : "wss://gateway.example"); }

void host_emit_node(esp_openclaw_node_handle_t node, esp_openclaw_node_event_t event)
{
    const esp_openclaw_node_disconnected_event_t disconnected = {
        .reason = ESP_OPENCLAW_NODE_DISCONNECTED_REASON_CONNECTION_LOST,
    };
    host_require(node->config.event_cb != NULL, "registered Node event callback");
    ++host.callback_depth;
    node->config.event_cb(node, event,
        event == ESP_OPENCLAW_NODE_EVENT_DISCONNECTED ? &disconnected : NULL,
        node->config.event_user_ctx);
    --host.callback_depth;
}

void host_emit_gateway(esp_openclaw_node_handle_t node, const char *event, const char *payload)
{
    host_require(node->config.gateway_event_cb != NULL, "registered Gateway event callback");
    char *borrowed = strdup(payload);
    host_require(borrowed != NULL, "event allocation");
    ++host.callback_depth;
    node->config.gateway_event_cb(node, event, borrowed, node->config.gateway_event_user_ctx);
    --host.callback_depth;
    free(borrowed); /* Match the Node API's callback-only payload lifetime. */
}

void host_command(esp_openclaw_node_handle_t node, const char *name)
{
    for (unsigned i = 0; i < node->command_count; ++i) {
        esp_openclaw_node_command_t *command = &node->commands[i];
        if (strcmp(command->name, name) != 0) continue;
        char *payload = NULL;
        esp_openclaw_node_error_t error = {0};
        ++host.callback_depth;
        esp_err_t result = command->handler(node, command->context, "{}", 2, &payload, &error);
        --host.callback_depth;
        host.last_started = payload != NULL && strstr(payload, "\"started\":true") != NULL;
        free(payload);
        host_require(result == ESP_OK, "registered command succeeds");
        return;
    }
    host_require(false, "command was never registered");
}

esp_err_t esp_openclaw_node_gateway_request(esp_openclaw_node_handle_t node,
    const char *method, const char *params, esp_openclaw_node_gateway_request_cb_t callback, void *ctx)
{
    host_require(host.critical_depth == 0 && mutex_depth == 0, "RPC outside locks");
    pending_rpc_t request = {.callback = callback, .ctx = ctx, .node = node};
    if (strcmp(method, "talk.config") == 0) {
        if (host.fail_config_submit) return ESP_FAIL;
        host_require(config_rpc.callback == NULL, "one config request outstanding");
        config_rpc = request;
        ++host.config_requests;
    } else if (strcmp(method, "talk.client.create") == 0) {
        if (host.fail_create_submit) return ESP_FAIL;
        host_require(create_rpc.callback == NULL, "one create request outstanding");
        cJSON *json = cJSON_Parse(params);
        cJSON *key = cJSON_GetObjectItemCaseSensitive(json, "sessionKey");
        host_require(cJSON_IsString(key) && strcmp(key->valuestring, "agent:fixture:main") == 0,
            "real Talk resolves synthetic Gateway routing");
        cJSON_Delete(json);
        create_rpc = request;
        ++host.create_requests;
    } else {
        host_require(strcmp(method, "talk.client.close") == 0, "unexpected RPC method");
        cJSON *json = cJSON_Parse(params);
        cJSON *voice = cJSON_GetObjectItemCaseSensitive(json, "voiceSessionId");
        cJSON *key = cJSON_GetObjectItemCaseSensitive(json, "sessionKey");
        host_require(cJSON_IsString(voice) && cJSON_IsString(key), "close contains real retained identity");
        host_require(strlen(voice->valuestring) < sizeof(host.close_voice) &&
            strlen(key->valuestring) < sizeof(host.close_key), "close fields fit");
        strcpy(host.close_voice, voice->valuestring);
        strcpy(host.close_key, key->valuestring);
        cJSON_Delete(json);
        host.close_node = node;
        ++host.close_requests;
    }
    return ESP_OK;
}

static void reply(pending_rpc_t *slot, bool ok, const char *payload)
{
    pending_rpc_t request = *slot;
    *slot = (pending_rpc_t){0};
    host_require(request.callback != NULL, "reply requires outstanding real RPC");
    char *borrowed = payload != NULL ? strdup(payload) : NULL;
    const esp_openclaw_node_gateway_result_t result = {
        .ok = ok, .payload_json = borrowed, .error_code = ok ? NULL : "UNAVAILABLE",
    };
    ++host.callback_depth;
    request.callback(request.node, &result, request.ctx);
    --host.callback_depth;
    free(borrowed);
}
void host_reply_config(void)
{
    reply(&config_rpc, true, "{\"config\":{\"talk\":{\"agentId\":\"fixture\"}}}");
}
void host_reply_create(bool ok)
{
    char payload[512];
    snprintf(payload, sizeof(payload),
        "{\"transport\":\"webrtc\",\"offerUrl\":\"/synthetic-offer\","
        "\"clientSecret\":\"synthetic-unused-token\",\"voiceSessionId\":\"%s\","
        "\"clientControl\":{\"owner\":\"gateway\"}}", host.create_voice != NULL ? host.create_voice : "voice-a");
    reply(&create_rpc, ok, ok ? payload : NULL);
}

const esp_peer_ops_t *esp_peer_get_default_impl(void)
{
    static const esp_peer_ops_t unused = {0};
    return &unused;
}
int esp_webrtc_open(esp_webrtc_cfg_t *config, esp_webrtc_handle_t *out)
{
    if (host.fail_open) return -1;
    fake_rtc_t *rtc = recycled_rtc != NULL ? recycled_rtc : calloc(1, sizeof(*rtc));
    recycled_rtc = NULL;
    host_require(rtc != NULL, "WebRTC allocation");
    memset(rtc, 0, sizeof(*rtc));
    rtc->config = *config;
    host_require(config->signaling_cfg.extra_size == sizeof(esp_openclaw_talk_call_handle_t),
        "prepared-call binding copies a pointer-sized reference");
    host_require(config->peer_cfg.extra_size == sizeof(esp_peer_default_cfg_t), "real peer aggregate size");
    host_require(!config->peer_cfg.enable_data_channel, "room config is audio only");
    /* The pinned SDK copies extra_size bytes; retain the exact public layout. */
    rtc->config.signaling_cfg.extra_cfg = malloc((size_t)config->signaling_cfg.extra_size);
    rtc->config.peer_cfg.extra_cfg = malloc((size_t)config->peer_cfg.extra_size);
    host_require(rtc->config.signaling_cfg.extra_cfg != NULL && rtc->config.peer_cfg.extra_cfg != NULL,
        "WebRTC config allocation");
    memcpy(rtc->config.signaling_cfg.extra_cfg, config->signaling_cfg.extra_cfg,
        (size_t)config->signaling_cfg.extra_size);
    memcpy(rtc->config.peer_cfg.extra_cfg, config->peer_cfg.extra_cfg, (size_t)config->peer_cfg.extra_size);
    *out = rtc;
    ++host.opens;
    return 0;
}
int esp_webrtc_set_media_provider(esp_webrtc_handle_t session, esp_webrtc_media_provider_t *provider)
{ host_require(session != NULL && provider->capture != NULL && provider->player != NULL, "media bound"); return host.fail_provider ? -1 : 0; }
int esp_webrtc_set_no_auto_capture(esp_webrtc_handle_t session, bool disable)
{ host_require(session != NULL && disable, "room owns capture"); return 0; }
int esp_webrtc_set_event_handler(esp_webrtc_handle_t session, esp_webrtc_event_handler_t callback, void *ctx)
{
    fake_rtc_t *rtc = session;
    rtc->handler = callback;
    rtc->handler_ctx = ctx;
    return 0;
}
static int signal_ice(esp_peer_signaling_ice_info_t *info, void *ctx)
{
    host_require(ctx != NULL && info->is_initiator, "real Talk ICE notification");
    ++host.ice_callbacks;
    return 0;
}
static int signal_connected(void *ctx)
{
    fake_rtc_t *rtc = ctx;
    rtc->ready = true;
    ++host.signaling_connected;
    return 0;
}
static int signal_closed(void *ctx)
{ host_require(ctx != NULL, "signaling context"); ++host.signaling_closed; return 0; }
static int signal_message(esp_peer_signaling_msg_t *message, void *ctx)
{ (void)message; (void)ctx; host_require(false, "no SDP exchange in this suite"); return -1; }
int esp_webrtc_start(esp_webrtc_handle_t session)
{
    if (host.fail_start) return -1;
    fake_rtc_t *rtc = session;
    esp_peer_signaling_cfg_t config = {
        .signal_url = rtc->config.signaling_cfg.signal_url,
        .extra_cfg = rtc->config.signaling_cfg.extra_cfg,
        .extra_size = rtc->config.signaling_cfg.extra_size,
        .on_ice_info = signal_ice, .on_connected = signal_connected,
        .on_close = signal_closed, .on_msg = signal_message, .ctx = rtc,
    };
    ++host.starts;
    host.inside_start = true;
    if (host.before_signaling_start != NULL) host.before_signaling_start();
    int result = rtc->config.signaling_impl->start(&config, &rtc->signaling);
    if (host.inside_webrtc_start != NULL) host.inside_webrtc_start();
    host.inside_start = false;
    return result;
}
void host_emit_peer(esp_webrtc_handle_t session, esp_webrtc_event_type_t type)
{
    fake_rtc_t *rtc = session;
    host_require(rtc->handler != NULL, "real room peer callback registered");
    if (type == ESP_WEBRTC_EVENT_CONNECTED) host_require(rtc->ready, "real Talk create must finish first");
    if (type == ESP_WEBRTC_EVENT_DISCONNECTED) ++host.peer_disconnects;
    esp_webrtc_event_t event = {.type = type};
    ++host.callback_depth;
    host_require(rtc->handler(&event, rtc->handler_ctx) == 0, "peer event handled");
    --host.callback_depth;
}
int esp_webrtc_close(esp_webrtc_handle_t session)
{
    fake_rtc_t *rtc = session;
    host_require(!host.inside_start && host.callback_depth == 0 && mutex_depth == 0,
        "canonical close must run outside start, callbacks and state lock");
    host_require(host.media_owned, "close while Talk still owns media");
    if (host.inside_close != NULL) host.inside_close();
    if (rtc->signaling != NULL) host_require(rtc->config.signaling_impl->stop(rtc->signaling) == 0, "real Talk stop");
    free(rtc->config.signaling_cfg.extra_cfg);
    free(rtc->config.peer_cfg.extra_cfg);
    if (host.reuse_rtc) recycled_rtc = rtc;
    else free(rtc);
    ++host.closes;
    return 0;
}

void room_media_begin_talk(void)
{
    host_require(!host.media_owned && host.ambient, "exclusive ambient-to-Talk media admission");
    host.media_owned = true;
    ++host.media_begins;
    if (host.at_media_begin != NULL) host.at_media_begin();
}
void room_media_end_talk(bool capture_remained_ambient)
{
    if (host.inside_media_end != NULL) host.inside_media_end();
    host_require(host.media_owned && host.ambient, "media released exactly once after ambient restore");
    if (!capture_remained_ambient) host_require(host.closes == host.media_ends + 1, "close precedes release");
    host.media_owned = false;
    ++host.media_ends;
}
esp_err_t room_media_set_ambient_wake(bool enabled)
{
    host_require(host.media_owned, "capture transfer requires media ownership");
    if (enabled) {
        if (host.inside_ambient_restore != NULL) host.inside_ambient_restore();
        host_require(!host.ambient && host.closes == host.media_ends + 1, "ambient restore follows close");
        ++host.ambient_restores;
    }
    host.ambient = enabled;
    return ESP_OK;
}
esp_err_t room_media_get_webrtc_provider(esp_webrtc_media_provider_t *provider)
{
    static int capture, player;
    *provider = (esp_webrtc_media_provider_t){.capture = &capture, .player = &player};
    return ESP_OK;
}
void room_ui_set(room_ui_state_t state, const char *detail)
{ (void)detail; host.ui = state; }
void room_ui_show_face_hint(uint32_t ms) { (void)ms; }
void room_ui_set_gateway(const char *gateway) { (void)gateway; }
bool room_ui_talk_face_active(void) { return host.ui == ROOM_UI_SPEAKING; }
void room_face_play_gesture(room_face_gesture_t gesture) { (void)gesture; }
void room_canvas_set_refresh_client(esp_openclaw_node_handle_t node) { (void)node; }
void room_canvas_set_gateway_http_base(const char *url) { (void)url; }
void room_canvas_set_node(esp_openclaw_node_handle_t node) { (void)node; }
esp_err_t room_canvas_register_node_commands(esp_openclaw_node_handle_t node) { (void)node; return ESP_OK; }
esp_err_t room_face_register_node_commands(esp_openclaw_node_handle_t node) { (void)node; return ESP_OK; }
esp_err_t room_device_register_node_commands(esp_openclaw_node_handle_t node) { (void)node; return ESP_OK; }
esp_err_t room_files_register_node_commands(esp_openclaw_node_handle_t node, const char *root)
{ (void)node; (void)root; host_require(false, "fixture has no storage"); return ESP_FAIL; }
const esp_openclaw_room_node_config_t *room_board_config(void)
{
    static const esp_openclaw_room_node_config_t board = {
        .display_name = "Synthetic room", .model_identifier = "host-fixture",
    };
    return &board;
}

/* Real HTTP header, intentionally no implementation capable of networking. */
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{ (void)config; host_require(false, "HTTP is forbidden in lifecycle host tests"); return NULL; }
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value)
{ (void)client; (void)key; (void)value; host_require(false, "unexpected HTTP header"); return ESP_FAIL; }
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client, const char *data, int length)
{ (void)client; (void)data; (void)length; host_require(false, "unexpected HTTP body"); return ESP_FAIL; }
esp_err_t esp_http_client_perform(esp_http_client_handle_t client)
{ (void)client; host_require(false, "HTTP is forbidden"); return ESP_FAIL; }
int esp_http_client_get_status_code(esp_http_client_handle_t client)
{ (void)client; host_require(false, "unexpected HTTP status"); return 0; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client)
{ (void)client; host_require(false, "unexpected HTTP cleanup"); return ESP_FAIL; }

void host_release_resources(void)
{
    host_require(config_rpc.callback == NULL && create_rpc.callback == NULL, "all real Talk RPC references drained");
    host_require(!host.media_owned && mutex_depth == 0 && host.critical_depth == 0, "fixture leaves no ownership");
    for (size_t i = 0; i < node_count; ++i) free(nodes[i]);
    free(queue->data);
    free(queue);
    free(mutex);
    free(timeout);
    free(operator_timer);
    free(recycled_rtc);
}
