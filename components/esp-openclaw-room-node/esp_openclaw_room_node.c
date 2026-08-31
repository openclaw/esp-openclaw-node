#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "cJSON.h"
#include "esp_console.h"
#include "esp_event.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_openclaw_node.h"
#include "esp_openclaw_node_example_repl.h"
#include "esp_openclaw_node_wifi.h"
#include "esp_openclaw_talk.h"
#include "esp_peer.h"
#include "esp_peer_default.h"
#include "esp_timer.h"
#include "esp_webrtc.h"
#include "esp_capture.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "nvs_flash.h"
#include "media_lib_adapter.h"
#include "media_lib_os.h"
#include "room_canvas.h"
#include "room_canvas_node_cmd.h"
#include "room_face.h"
#include "room_device_commands.h"
#include "room_diagnostics.h"
#include "room_diagnostics_data.h"
#include "room_files.h"
#include "room_latency_policy.h"
#include "room_media.h"
#include "room_runtime_diagnostics.h"
#include "room_ui_controller.h"
#include "room_board.h"
#include "esp_openclaw_room_node.h"

static const char *TAG = "openclaw_room";
static esp_openclaw_node_handle_t node_client;
static esp_openclaw_node_handle_t operator_client;
static esp_webrtc_handle_t webrtc;
static SemaphoreHandle_t state_lock;
static QueueHandle_t talk_teardown_queue;
static TimerHandle_t talk_timeout_timer;
static bool operator_ready;
static bool node_ready;
static bool media_ready;
static bool talk_start_in_flight;
static bool talk_dialing;
static bool talk_active;
static bool talk_closing;
/* Durable until the next admission, including after start has returned. */
static bool talk_cancel_requested;
static const char *talk_cancel_message;
static uint32_t talk_generation;
static uint64_t operator_incarnation;
static uint64_t talk_operator_incarnation;
static esp_openclaw_node_handle_t talk_operator;
static esp_openclaw_talk_call_handle_t talk_call;
static bool talk_media_owned;
static bool talk_ambient_suspended;
static bool talk_speaking_pending;
static bool talk_connecting_pending;
static bool operator_ui_pending;
static bool node_reconnect_scheduled;
static bool camera_active;
static char *gateway_http_base;

/* A one-slot wakeup has no identity or action to become stale. The durable,
 * generation-checked facts under state_lock are the only work authority. */
typedef uint8_t talk_teardown_request_t;

static void media_scheduler(const char *name, media_lib_thread_cfg_t *cfg)
{
    if (strcmp(name, "aenc_0") == 0 || strcmp(name, "AUD_SRC") == 0) {
        cfg->stack_size = 40 * 1024;
        cfg->priority = strcmp(name, "aenc_0") == 0 ? 10 : 15;
        cfg->core_id = 1;
    } else if (strcmp(name, "buffer_in") == 0) {
        cfg->stack_size = 6 * 1024;
        cfg->priority = 10;
        cfg->core_id = 0;
    } else if (strcmp(name, "pc_task") == 0) {
        cfg->stack_size = 25 * 1024;
        cfg->priority = 18;
        cfg->core_id = 1;
    } else if (strcmp(name, "pc_send") == 0) {
        cfg->stack_size = 4 * 1024;
        cfg->priority = 15;
        cfg->core_id = 1;
    } else if (strcmp(name, "Adec") == 0) {
        cfg->stack_size = 40 * 1024;
        cfg->priority = 15;
        cfg->core_id = 0;
    } else if (strcmp(name, "ARender") == 0) {
        cfg->priority = 20;
    }
}

static void capture_scheduler(const char *name, esp_capture_thread_schedule_cfg_t *cfg)
{
    media_lib_thread_cfg_t media = {
        .stack_size = cfg->stack_size,
        .priority = cfg->priority,
        .core_id = cfg->core_id,
    };
    media_scheduler(name, &media);
    cfg->stack_size = media.stack_size;
    cfg->priority = media.priority;
    cfg->core_id = media.core_id;
    cfg->stack_in_ext = true;
}

static void start_talk_once(void);
static esp_err_t register_talk_node_commands(esp_openclaw_node_handle_t node);
static TaskHandle_t talk_start_worker;
/* One-shot timer that (re)tries creating the transient operator-start task.
 * A permanent worker task was tried and reverted: its always-resident internal
 * stack starved the AFE open/close sync-tasks during calls (internal RAM is
 * the scarcest resource on this board). Guarded by state_lock. */
static esp_timer_handle_t operator_start_timer;
static bool operator_start_scheduled;

static void talk_start_worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        start_talk_once();
    }
}

static char *gateway_http_base_from_uri(const char *gateway_uri)
{
    if (gateway_uri == NULL) {
        return NULL;
    }
    const char *authority = NULL;
    const char *http_scheme = NULL;
    if (strncmp(gateway_uri, "ws://", 5) == 0) {
        authority = gateway_uri + 5;
        http_scheme = "http://";
    } else if (strncmp(gateway_uri, "wss://", 6) == 0) {
        authority = gateway_uri + 6;
        http_scheme = "https://";
    } else {
        return NULL;
    }
    const char *path = strchr(authority, '/');
    size_t authority_len = path != NULL ? (size_t)(path - authority) : strlen(authority);
    size_t required = strlen(http_scheme) + authority_len + 1;
    char *base = malloc(required);
    if (base != NULL) {
        snprintf(base, required, "%s%.*s", http_scheme, (int)authority_len, authority);
    }
    return base;
}

static void operator_start_task(void *arg);

/* Runs on the esp_timer task: only a task-create attempt, nothing heavy.
 * Creation can fail at the busiest boot moment (internal RAM contention with
 * Wi-Fi/TLS/audio); re-arming the timer turns that into a delay instead of a
 * silent dead-end. The task itself is transient so its internal stack is
 * returned between attempts — a permanent worker starved the AFE sync-tasks. */
static void operator_start_timer_fired(void *arg)
{
    (void)arg;
    if (xTaskCreate(operator_start_task, "operator_start", 6144, NULL, 6, NULL) == pdPASS) {
        return;
    }
    ESP_LOGW(TAG, "operator start task creation failed; retrying in 2s");
    esp_timer_start_once(operator_start_timer, 2000000);
}

static bool schedule_operator_start(uint32_t delay_ms)
{
    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool schedule = !operator_start_scheduled;
    if (schedule) {
        operator_start_scheduled = true;
    }
    xSemaphoreGive(state_lock);
    if (!schedule) {
        return true;
    }
    if (operator_start_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = operator_start_timer_fired,
            .name = "operator_start",
        };
        if (esp_timer_create(&args, &operator_start_timer) != ESP_OK) {
            xSemaphoreTake(state_lock, portMAX_DELAY);
            operator_start_scheduled = false;
            xSemaphoreGive(state_lock);
            return false;
        }
    }
    esp_timer_stop(operator_start_timer);
    if (esp_timer_start_once(operator_start_timer, (uint64_t)delay_ms * 1000) != ESP_OK) {
        xSemaphoreTake(state_lock, portMAX_DELAY);
        operator_start_scheduled = false;
        xSemaphoreGive(state_lock);
        return false;
    }
    return true;
}

/*
 * Kconfig Wi-Fi credentials seed the shared NVS-backed station helper only
 * while it is unconfigured, so `wifi set` from the USB console always wins
 * across reboots and re-provisioning never needs a rebuild.
 */
static void seed_wifi_credentials_from_kconfig(void)
{
    if (CONFIG_OPENCLAW_ROOM_WIFI_SSID[0] == '\0') {
        return;
    }
    esp_openclaw_node_wifi_status_t status = {0};
    esp_openclaw_node_wifi_get_status(&status);
    if (status.has_saved_network) {
        return;
    }
    esp_err_t err = esp_openclaw_node_wifi_set_credentials(
        CONFIG_OPENCLAW_ROOM_WIFI_SSID,
        CONFIG_OPENCLAW_ROOM_WIFI_PASSWORD);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "seeding Wi-Fi credentials failed: %s", esp_err_to_name(err));
    }
}

static void wake_talk_teardown(void)
{
    const talk_teardown_request_t wake = 1;
    xQueueOverwrite(talk_teardown_queue, &wake);
}

/* Caller holds state_lock. Cancellation is durable, including during admission
 * and media-gate waits; no later CONNECTED event can undo it. */
static bool request_talk_stop_locked(uint32_t generation, const char *message)
{
    if (talk_call == NULL || generation != talk_generation || talk_closing) return false;
    if (!talk_cancel_requested) talk_cancel_message = message;
    talk_cancel_requested = true;
    esp_openclaw_talk_call_cancel(talk_call);
    wake_talk_teardown();
    return true;
}

static void request_talk_teardown(uint32_t generation, const char *message)
{
    xSemaphoreTake(state_lock, portMAX_DELAY);
    request_talk_stop_locked(generation, message);
    xSemaphoreGive(state_lock);
}

static void talk_teardown_task(void *arg)
{
    (void)arg;
    for (;;) {
        talk_teardown_request_t wake;
        if (xQueueReceive(talk_teardown_queue, &wake, portMAX_DELAY) != pdTRUE) continue;
        xSemaphoreTake(state_lock, portMAX_DELAY);
        bool close = talk_call != NULL && talk_cancel_requested &&
            !talk_start_in_flight && !talk_closing;
        bool speaking = talk_speaking_pending && !talk_cancel_requested && !talk_closing;
        bool connecting = talk_connecting_pending && !talk_active && !talk_cancel_requested && !talk_closing;
        bool operator_ui = operator_ui_pending && talk_call == NULL;
        bool ready = operator_ready;
        talk_speaking_pending = false;
        talk_connecting_pending = false;
        operator_ui_pending = false;
        if (close) talk_closing = true;
        esp_webrtc_handle_t session = webrtc;
        esp_openclaw_talk_call_handle_t call = talk_call;
        const char *message = talk_cancel_message;
        xSemaphoreGive(state_lock);
        /* One worker serializes call/operator UI. A delayed callback never
         * paints over a replacement call's newer connecting/speaking state. */
        if (speaking) room_ui_set(ROOM_UI_SPEAKING, NULL);
        else if (connecting) {
            room_ui_set(ROOM_UI_CONNECTING, NULL);
            room_face_play_gesture(ROOM_FACE_GESTURE_SURPRISE);
        } else if (operator_ui) {
            room_ui_set(ready ? ROOM_UI_IDLE : ROOM_UI_ERROR, ready ? NULL : "Operator offline");
            if (ready) room_ui_show_face_hint(8000);
        }
        if (!close) continue;

        /* The owner remains reserved until every local resource and UI update
         * is finished. Pending RPC refs may survive, but cannot dispatch. */
        esp_openclaw_talk_call_quiesce(call);
        if (talk_timeout_timer != NULL) {
            xTimerStop(talk_timeout_timer, portMAX_DELAY);
            xTimerDelete(talk_timeout_timer, portMAX_DELAY);
            talk_timeout_timer = NULL;
        }
        if (session != NULL) esp_webrtc_close(session);
        if (talk_ambient_suspended && room_media_set_ambient_wake(true) != ESP_OK) {
            ESP_LOGE(TAG, "failed to restore ambient WakeNet after Talk");
            message = "Talk failed";
        }
        if (talk_media_owned) room_media_end_talk(!talk_ambient_suspended);
        room_ui_set(message != NULL ? ROOM_UI_ERROR : ROOM_UI_IDLE, message);
        xSemaphoreTake(state_lock, portMAX_DELAY);
        webrtc = NULL;
        talk_call = NULL;
        talk_operator = NULL;
        talk_dialing = false;
        talk_active = false;
        talk_media_owned = false;
        talk_ambient_suspended = false;
        talk_closing = false;
        xSemaphoreGive(state_lock);
        esp_openclaw_talk_call_release(call);
    }
}

static void call_timeout(TimerHandle_t timer)
{
    request_talk_teardown((uint32_t)(uintptr_t)pvTimerGetTimerID(timer), NULL);
}

static void talk_setup_failed(esp_openclaw_talk_setup_result_t result, void *ctx)
{
    request_talk_teardown((uint32_t)(uintptr_t)ctx,
        result == ESP_OPENCLAW_TALK_GATEWAY_UPGRADE_REQUIRED
            ? "Gateway upgrade required" : "Talk setup failed");
}

static void talk_session_closed(void *ctx)
{
    request_talk_teardown((uint32_t)(uintptr_t)ctx, NULL);
}

static int webrtc_event(esp_webrtc_event_t *event, void *ctx)
{
    uint32_t generation = (uint32_t)(uintptr_t)ctx;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool current = talk_call != NULL && generation == talk_generation &&
        !talk_cancel_requested && !talk_closing;
    if (current && event->type == ESP_WEBRTC_EVENT_CONNECTED) {
        talk_dialing = false;
        talk_active = true;
        talk_speaking_pending = true;
        wake_talk_teardown();
    } else if (event->type == ESP_WEBRTC_EVENT_CONNECT_FAILED ||
               event->type == ESP_WEBRTC_EVENT_DISCONNECTED) {
        request_talk_stop_locked(generation,
            event->type == ESP_WEBRTC_EVENT_CONNECT_FAILED ? "Talk failed" : NULL);
    }
    xSemaphoreGive(state_lock);
    return 0;
}

/* Called under state_lock at wake/command admission, never in the worker. */
static bool admit_talk_locked(void)
{
    if (!operator_ready || operator_client == NULL || gateway_http_base == NULL ||
        camera_active || talk_call != NULL || talk_generation == UINT32_MAX) return false;
    uint32_t generation = talk_generation + 1;
    esp_openclaw_talk_signaling_config_t signaling = {
        .operator_node = operator_client,
        .gateway_http_base_url = gateway_http_base,
        .provider = CONFIG_OPENCLAW_ROOM_TALK_PROVIDER,
        .model = CONFIG_OPENCLAW_ROOM_TALK_MODEL,
        .voice = CONFIG_OPENCLAW_ROOM_TALK_VOICE,
        .setup_failed_cb = talk_setup_failed,
        .setup_failed_ctx = (void *)(uintptr_t)generation,
        .silence_duration_ms = ROOM_TALK_VAD_SILENCE_MS,
    };
    esp_openclaw_talk_call_handle_t call = NULL;
    if (esp_openclaw_talk_call_prepare(&signaling, &call) != ESP_OK) return false;
    talk_timeout_timer = xTimerCreate("talk_timeout",
        pdMS_TO_TICKS(CONFIG_OPENCLAW_ROOM_CALL_IDLE_SECONDS * 1000), pdFALSE,
        (void *)(uintptr_t)generation, call_timeout);
    if (talk_timeout_timer == NULL) {
        esp_openclaw_talk_call_release(call);
        return false;
    }
    esp_openclaw_talk_call_set_closed_handler(call, talk_session_closed, (void *)(uintptr_t)generation);
    talk_call = call;
    talk_operator = operator_client;
    talk_operator_incarnation = operator_incarnation;
    talk_generation = generation;
    talk_start_in_flight = true;
    talk_cancel_requested = false;
    talk_cancel_message = NULL;
    talk_connecting_pending = true;
    wake_talk_teardown();
    return true;
}

static void start_talk_once(void)
{
    xSemaphoreTake(state_lock, portMAX_DELAY);
    esp_openclaw_talk_call_handle_t call = talk_call;
    uint32_t generation = talk_generation;
    bool cancelled = talk_cancel_requested;
    bool admitted = call != NULL && talk_start_in_flight;
    xSemaphoreGive(state_lock);
    if (!admitted) return;
    if (cancelled) goto done;
    room_media_begin_talk();
    xSemaphoreTake(state_lock, portMAX_DELAY);
    talk_media_owned = true;
    cancelled = talk_cancel_requested;
    xSemaphoreGive(state_lock);
    if (cancelled) goto done;

    esp_peer_default_cfg_t peer = {.agent_recv_timeout = 500, .ice_use_lite_mode = true};
    esp_webrtc_cfg_t config = {
        .peer_cfg = {
            .audio_info = {.codec = ESP_PEER_AUDIO_CODEC_OPUS, .sample_rate = 16000, .channel = 1},
            .audio_dir = ESP_PEER_MEDIA_DIR_SEND_RECV,
            .enable_data_channel = false,
            .extra_cfg = &peer,
            .extra_size = sizeof(peer),
        },
        .signaling_cfg = {.extra_cfg = &call, .extra_size = sizeof(call)},
        .peer_impl = esp_peer_get_default_impl(),
        .signaling_impl = esp_openclaw_talk_call_signaling_impl(),
    };
    esp_webrtc_handle_t session = NULL;
    if (esp_webrtc_open(&config, &session) != 0) {
        request_talk_teardown(generation, "WebRTC open");
        goto done;
    }
    xSemaphoreTake(state_lock, portMAX_DELAY);
    webrtc = session;
    talk_dialing = true;
    xSemaphoreGive(state_lock);
    esp_webrtc_media_provider_t media = {0};
    if (room_media_get_webrtc_provider(&media) != ESP_OK ||
        esp_webrtc_set_media_provider(session, &media) != 0 ||
        esp_webrtc_set_no_auto_capture(session, true) != 0 ||
        esp_webrtc_set_event_handler(session, webrtc_event, (void *)(uintptr_t)generation) != 0) {
        request_talk_teardown(generation, "WebRTC start");
        goto done;
    }
    xSemaphoreTake(state_lock, portMAX_DELAY);
    cancelled = talk_cancel_requested;
    if (!cancelled) talk_ambient_suspended = true;
    xSemaphoreGive(state_lock);
    if (cancelled) goto done;
    if (room_media_set_ambient_wake(false) != ESP_OK || esp_webrtc_start(session) != 0 ||
        xTimerStart(talk_timeout_timer, 0) != pdPASS) {
        request_talk_teardown(generation, "Talk setup failed");
    }

done:
    xSemaphoreTake(state_lock, portMAX_DELAY);
    talk_start_in_flight = false;
    if (talk_cancel_requested) wake_talk_teardown();
    xSemaphoreGive(state_lock);
}

static void on_wake(const char *wake_word, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "ambient wake word detected: %s", wake_word);
    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool start = admit_talk_locked();
    xSemaphoreGive(state_lock);
    if (!start) {
        return;
    }
    xTaskNotifyGive(talk_start_worker);
}

static void operator_gateway_event(
    esp_openclaw_node_handle_t client,
    const char *event,
    const char *payload_json,
    void *ctx)
{
    (void)ctx;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    esp_openclaw_talk_call_handle_t call = talk_call;
    bool bound = call != NULL && client == talk_operator && !talk_cancel_requested && !talk_closing &&
        (client != operator_client || talk_operator_incarnation == operator_incarnation);
    if (bound) esp_openclaw_talk_call_retain(call);
    xSemaphoreGive(state_lock);
    if (bound) {
        esp_openclaw_talk_call_gateway_event(call, client, event, payload_json);
        esp_openclaw_talk_call_release(call);
    }
    if (event != NULL && (strcmp(event, "voicewake.changed") == 0 ||
        strcmp(event, "voicewake.routing.changed") == 0)) {
        ESP_LOGI(TAG, "Gateway wake routing updated: %s", payload_json);
    }
}

static void operator_event(
    esp_openclaw_node_handle_t client,
    esp_openclaw_node_event_t event,
    const void *data,
    void *ctx)
{
    (void)data;
    (void)ctx;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool current = client == operator_client;
    bool connected = event == ESP_OPENCLAW_NODE_EVENT_CONNECTED;
    /* A displaced operator can still own the admitted call. Its loss cancels
     * that owner, but has no authority over the replacement operator's UI. */
    if (!connected && client == talk_operator) request_talk_stop_locked(talk_generation, NULL);
    if (current) {
        operator_ready = connected;
        if (connected) {
            ++operator_incarnation;
            if (client == talk_operator && talk_operator_incarnation != operator_incarnation) {
                request_talk_stop_locked(talk_generation, NULL);
            }
        }
    }
    bool ready = current && operator_ready;
    if (current) {
        operator_ui_pending = true;
        wake_talk_teardown();
    }
    xSemaphoreGive(state_lock);
    if (!current) {
        return;
    }
    if (ready) {
        ESP_LOGI(TAG, "operator Talk session ready");
    } else {
        (void)schedule_operator_start(2000);
    }
}

static esp_err_t start_operator_client(void)
{
    xSemaphoreTake(state_lock, portMAX_DELAY);
    esp_openclaw_node_handle_t existing = operator_client;
    xSemaphoreGive(state_lock);
    if (existing != NULL) {
        /* No has_saved_session precheck: the component reloads NVS on every
         * saved-session connect, so a handoff token persisted later by the
         * node client (console re-pair) is picked up here. The precheck read
         * a stale in-memory snapshot and wedged this loop forever. */
        const esp_openclaw_node_connect_request_t reconnect = {
            .source = ESP_OPENCLAW_NODE_CONNECT_SOURCE_SAVED_SESSION,
        };
        esp_err_t err = esp_openclaw_node_request_connect(existing, &reconnect);
        return err == ESP_ERR_INVALID_STATE ? ESP_OK : err;
    }

    esp_openclaw_node_config_t config = {0};
    esp_openclaw_node_config_init_default(&config);
    config.display_name = "OpenClaw Room Talk";
    config.role = "operator";
    config.event_cb = operator_event;
    config.gateway_event_cb = operator_gateway_event;
    esp_openclaw_node_handle_t created = NULL;
    ESP_RETURN_ON_ERROR(esp_openclaw_node_create(&config, &created), TAG, "operator client create");
    esp_err_t err = esp_openclaw_node_register_scope(created, "operator.read");
    if (err == ESP_OK) {
        err = esp_openclaw_node_register_scope(created, "operator.talk");
    }
    if (err != ESP_OK) {
        esp_openclaw_node_destroy(created);
        return err;
    }
    const esp_openclaw_node_connect_request_t request = {
        .source = ESP_OPENCLAW_NODE_CONNECT_SOURCE_SAVED_SESSION,
    };
    xSemaphoreTake(state_lock, portMAX_DELAY);
    operator_client = created;
    xSemaphoreGive(state_lock);
    /* The canvas keep-warm refresh needs operator scope. */
    room_canvas_set_refresh_client(created);
    return esp_openclaw_node_request_connect(created, &request);
}

static void operator_start_task(void *arg)
{
    (void)arg;
    /* Release the scheduled flag before the attempt so a synchronous failure
     * can re-arm the next retry. */
    xSemaphoreTake(state_lock, portMAX_DELAY);
    operator_start_scheduled = false;
    xSemaphoreGive(state_lock);
    esp_err_t err = start_operator_client();
    if (err != ESP_OK) {
        // Every retry logs: the display alone cannot say WHY the operator
        // session is down, and a silent loop here cost a debugging session.
        ESP_LOGW(TAG, "operator client start failed: %s (retrying)", esp_err_to_name(err));
        room_ui_set(
            ROOM_UI_ERROR,
            err == ESP_ERR_NOT_FOUND ? "No operator session\nre-pair via console" : "Operator token");
        (void)schedule_operator_start(5000);
    }
    vTaskDelete(NULL);
}

static esp_err_t request_node_connection(void)
{
    /* Saved session first (the component reloads it from NVS per request);
     * only a NOT_FOUND answer falls back to the baked setup code so a live
     * console-provisioned session is never clobbered by stale Kconfig. */
    esp_openclaw_node_connect_request_t request = {
        .source = ESP_OPENCLAW_NODE_CONNECT_SOURCE_SAVED_SESSION,
    };
    esp_err_t err = esp_openclaw_node_request_connect(node_client, &request);
    if (err != ESP_ERR_NOT_FOUND) {
        return err;
    }
    if (CONFIG_OPENCLAW_ROOM_SETUP_CODE[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }
    request.source = ESP_OPENCLAW_NODE_CONNECT_SOURCE_SETUP_CODE;
    request.value = CONFIG_OPENCLAW_ROOM_SETUP_CODE;
    return esp_openclaw_node_request_connect(node_client, &request);
}

static void schedule_node_reconnect(uint32_t delay_ms);

static void node_reconnect_task(void *arg)
{
    uint32_t delay_ms = (uint32_t)(uintptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    xSemaphoreTake(state_lock, portMAX_DELAY);
    node_reconnect_scheduled = false;
    xSemaphoreGive(state_lock);
    esp_err_t err = request_node_connection();
    if (err == ESP_ERR_NOT_FOUND) {
        /* No saved session and no baked setup code: provisioning is a console
         * action, so retrying here would loop forever with no new input. */
        room_ui_set(ROOM_UI_SETUP, "USB console:\ngateway setup-code");
        vTaskDelete(NULL);
        return;
    }
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        schedule_node_reconnect(5000);
    }
    vTaskDelete(NULL);
}

static void schedule_node_reconnect(uint32_t delay_ms)
{
    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool schedule = !node_reconnect_scheduled;
    if (schedule) {
        node_reconnect_scheduled = true;
    }
    xSemaphoreGive(state_lock);
    if (schedule &&
        xTaskCreate(
            node_reconnect_task,
            "node_reconnect",
            4096,
            (void *)(uintptr_t)delay_ms,
            5,
            NULL) != pdPASS) {
        xSemaphoreTake(state_lock, portMAX_DELAY);
        node_reconnect_scheduled = false;
        xSemaphoreGive(state_lock);
    }
}

static void node_event(
    esp_openclaw_node_handle_t client,
    esp_openclaw_node_event_t event,
    const void *data,
    void *ctx)
{
    (void)client;
    (void)data;
    (void)ctx;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    node_ready = event == ESP_OPENCLAW_NODE_EVENT_CONNECTED;
    xSemaphoreGive(state_lock);
    if (event == ESP_OPENCLAW_NODE_EVENT_CONNECTED) {
        /*
         * The session URI lands in NVS only after the first hello-ok, so the
         * boot-time base derivation is stale after console provisioning or a
         * re-pair to a different gateway. Refresh it on every connect.
         */
        char *gateway_uri = esp_openclaw_node_dup_gateway_uri(node_client);
        char *new_http_base = CONFIG_OPENCLAW_ROOM_GATEWAY_HTTP_BASE_URL[0] != '\0'
            ? strdup(CONFIG_OPENCLAW_ROOM_GATEWAY_HTTP_BASE_URL)
            : gateway_http_base_from_uri(gateway_uri);
        if (new_http_base == NULL) {
            ESP_LOGE(TAG, "Gateway HTTP origin is unavailable");
            room_ui_set(ROOM_UI_ERROR, "Gateway URL");
            free(gateway_uri);
            return;
        }
        char *canvas_http_base = strdup(new_http_base);
        xSemaphoreTake(state_lock, portMAX_DELAY);
        char *old_http_base = gateway_http_base;
        bool origin_changed = old_http_base != NULL && strcmp(old_http_base, new_http_base) != 0;
        gateway_http_base = new_http_base;
        if (origin_changed) request_talk_stop_locked(talk_generation, NULL);
        esp_openclaw_node_handle_t existing_operator = operator_client;
        xSemaphoreGive(state_lock);
        free(old_http_base);
        room_canvas_set_gateway_http_base(canvas_http_base);
        free(canvas_http_base);

        if (existing_operator != NULL) {
            char *operator_uri = esp_openclaw_node_dup_gateway_uri(existing_operator);
            bool gateway_changed = origin_changed || (operator_uri != NULL && gateway_uri != NULL &&
                strcmp(operator_uri, gateway_uri) != 0);
            free(operator_uri);
            if (gateway_changed) {
                ESP_LOGI(TAG, "Gateway changed; reconnecting the operator client");
                xSemaphoreTake(state_lock, portMAX_DELAY);
                operator_ready = false;
                request_talk_stop_locked(talk_generation, NULL);
                xSemaphoreGive(state_lock);
                esp_err_t disconnect_err =
                    esp_openclaw_node_request_disconnect(existing_operator);
                if (disconnect_err != ESP_OK && disconnect_err != ESP_ERR_INVALID_STATE) {
                    ESP_LOGW(TAG, "operator reconnect request failed: %s",
                        esp_err_to_name(disconnect_err));
                }
            }
        }
        /* Show host:port under the face; the saved URI is authoritative for
         * which gateway this session actually authenticated against. */
        const char *host = gateway_uri;
        if (host != NULL && strncmp(host, "ws://", 5) == 0) {
            host += 5;
        } else if (host != NULL && strncmp(host, "wss://", 6) == 0) {
            host += 6;
        }
        room_ui_set_gateway(host);
        free(gateway_uri);
        /* The timer path retries task creation itself, so scheduling only
         * fails on timer-create OOM at boot; retry through the same path. */
        if (!schedule_operator_start(0)) {
            ESP_LOGE(TAG, "failed scheduling operator client start; retrying in 5s");
            room_ui_set(ROOM_UI_ERROR, "Operator token");
            (void)schedule_operator_start(5000);
        }
    } else {
        room_ui_set_gateway(NULL);
        /* The node role does not own Talk control. Its disconnect reconnects
         * quietly; loss of the bound operator is handled separately. */
        if (!room_ui_talk_face_active()) {
            room_ui_set(ROOM_UI_ERROR, "Node offline");
        }
        schedule_node_reconnect(2000);
    }
}

static esp_err_t start_node_client(void)
{
    esp_openclaw_node_config_t config = {0};
    esp_openclaw_node_config_init_default(&config);
    const esp_openclaw_room_node_config_t *board = room_board_config();
    config.display_name = board->display_name;
    config.model_identifier = board->model_identifier;
    config.event_cb = node_event;
    ESP_RETURN_ON_ERROR(esp_openclaw_node_create(&config, &node_client), TAG, "node create");
    /* Advertising audio surfaces the microphone path cannot serve would route
     * Talk to a node that silently fails; Canvas still works without them. */
    if (media_ready) {
        ESP_RETURN_ON_ERROR(
            esp_openclaw_node_register_capability(node_client, "talk"),
            TAG,
            "talk capability");
        ESP_RETURN_ON_ERROR(
            esp_openclaw_node_register_capability(node_client, "voiceWake"),
            TAG,
            "wake capability");
        ESP_RETURN_ON_ERROR(
            register_talk_node_commands(node_client),
            TAG,
            "talk commands");
    } else {
        ESP_LOGW(TAG, "media init failed; advertising canvas only (no talk/voiceWake)");
    }
    ESP_RETURN_ON_ERROR(
        esp_openclaw_node_register_capability(node_client, "canvas"),
        TAG,
        "canvas capability");
    ESP_RETURN_ON_ERROR(
        room_canvas_register_node_commands(node_client),
        TAG,
        "canvas commands");
    ESP_RETURN_ON_ERROR(
        room_face_register_node_commands(node_client),
        TAG,
        "face commands");
    ESP_RETURN_ON_ERROR(
        room_device_register_node_commands(node_client),
        TAG,
        "device commands");
    bool storage_available = board->storage.public_root != NULL &&
        (board->storage.is_available == NULL ||
         board->storage.is_available(board->storage.ctx));
    if (storage_available) {
        ESP_RETURN_ON_ERROR(
            room_files_register_node_commands(node_client, board->storage.public_root),
            TAG,
            "file commands");
    }
    if (board->services.register_commands != NULL) {
        ESP_RETURN_ON_ERROR(
            board->services.register_commands(board->services.ctx, node_client),
            TAG,
            "board commands");
    }
    room_canvas_set_node(node_client);
    return ESP_OK;
}

/*
 * Bidirectional talk: the wake word starts a session from the room, and these
 * node commands let the agent start or end one remotely. Both directions run
 * the same Gateway-controlled WebRTC flow.
 */
static esp_err_t handle_talk_start(
    esp_openclaw_node_handle_t node,
    void *context,
    const char *params_json,
    size_t params_len,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    (void)node;
    (void)context;
    cJSON *params = cJSON_ParseWithLength(params_json, params_len);
    bool params_valid = cJSON_IsObject(params) && params->child == NULL;
    cJSON_Delete(params);
    if (!params_valid) {
        out_error->code = "INVALID_PARAMS";
        out_error->message = "talk.start accepts only {}";
        return ESP_ERR_INVALID_ARG;
    }
    if (!media_ready) {
        out_error->code = "UNAVAILABLE";
        out_error->message = "the microphone path failed at boot; talk is disabled on this node";
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool already_active = camera_active || talk_call != NULL;
    bool start = admit_talk_locked();
    bool operator_offline = !operator_ready;
    xSemaphoreGive(state_lock);
    if (operator_offline) {
        out_error->code = "UNAVAILABLE";
        out_error->message = "the operator session is offline; the node reconnects automatically";
        return ESP_ERR_INVALID_STATE;
    }
    if (!start) {
        if (!already_active) {
            out_error->code = "INTERNAL";
            out_error->message = "could not prepare Talk";
            return ESP_ERR_NO_MEM;
        }
        *out_payload_json = strdup("{\"started\":false,\"alreadyActive\":true}");
    } else {
        xTaskNotifyGive(talk_start_worker);
        *out_payload_json = strdup("{\"started\":true}");
    }
    if (*out_payload_json == NULL) {
        out_error->code = "INTERNAL";
        out_error->message = "not enough memory for the command result";
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t handle_talk_stop(
    esp_openclaw_node_handle_t node,
    void *context,
    const char *params_json,
    size_t params_len,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    (void)node;
    (void)context;
    cJSON *params = cJSON_ParseWithLength(params_json, params_len);
    bool params_valid = cJSON_IsObject(params) && params->child == NULL;
    cJSON_Delete(params);
    if (!params_valid) {
        out_error->code = "INVALID_PARAMS";
        out_error->message = "talk.stop accepts only {}";
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool active = request_talk_stop_locked(talk_generation, NULL);
    xSemaphoreGive(state_lock);
    *out_payload_json = strdup(
        active ? "{\"stopped\":true}" : "{\"stopped\":false}");
    if (*out_payload_json == NULL) {
        out_error->code = "INTERNAL";
        out_error->message = "not enough memory for the command result";
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t register_talk_node_commands(esp_openclaw_node_handle_t node)
{
    static const esp_openclaw_node_command_t COMMANDS[] = {
        {.name = "talk.start", .handler = handle_talk_start},
        {.name = "talk.stop", .handler = handle_talk_stop},
    };
    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); ++i) {
        ESP_RETURN_ON_ERROR(
            esp_openclaw_node_register_command(node, &COMMANDS[i]),
            TAG,
            "register %s",
            COMMANDS[i].name);
    }
    return ESP_OK;
}

static int wake_console_command(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!media_ready) {
        printf("talk is unavailable: the microphone path failed at boot\n");
        return 1;
    }
    printf("simulating wake word; watch the display for Talk state\n");
    on_wake("console", NULL);
    return 0;
}

static int diagnostics_display_command(bool open)
{
    esp_err_t err = open
        ? room_diagnostics_request_open()
        : room_diagnostics_request_close();
    if (err != ESP_OK) {
        printf("diagnostics %s failed: %s\n", open ? "open" : "close", esp_err_to_name(err));
        return 1;
    }
    printf("diagnostics %s queued\n", open ? "open" : "close");
    return 0;
}

static int diagnostics_tone_command(void)
{
    esp_err_t err = room_runtime_request_test_tone();
    room_media_tone_snapshot_t tone = {0};
    room_media_get_tone_snapshot(&tone);
    if (err == ESP_OK) {
        printf("diagnostics tone queued\n");
        return 0;
    }
    if (tone.state == ROOM_MEDIA_TONE_BUSY || tone.state == ROOM_MEDIA_TONE_RUNNING) {
        printf("diagnostics tone busy\n");
    } else if (tone.error == ROOM_MEDIA_TONE_ERROR_UNAVAILABLE) {
        printf("diagnostics tone unavailable\n");
    } else if (tone.error == ROOM_MEDIA_TONE_ERROR_TASK) {
        printf("diagnostics tone failed to create worker\n");
    } else {
        printf("diagnostics tone request failed: %s\n", esp_err_to_name(err));
    }
    return 1;
}

static int diagnostics_status_command(void)
{
    room_audio_diagnostics_snapshot_t audio = {0};
    room_diagnostics_audio_get(&audio);
    room_runtime_diagnostics_snapshot_t runtime = {0};
    room_runtime_get_diagnostics(&runtime);
    room_media_tone_snapshot_t tone = {0};
    room_media_get_tone_snapshot(&tone);

    printf("diagnostics=%s Talk=%s tone=%s",
        runtime.diagnostics_open ? "open" : "closed",
        runtime.talk_phase,
        room_media_tone_state_name(tone.state));
    if (tone.state == ROOM_MEDIA_TONE_ERROR) {
        printf("/%s", room_media_tone_error_name(tone.error));
    }
    printf(" (%u/%u/%u requested/queued/accepted)\n",
        tone.requested_frames,
        tone.enqueued_frames,
        tone.renderer_accepted_frames);
    printf("audio mic=%u@%" PRId64 "us afe=%u@%" PRId64
           "us rx=%u@%" PRId64 "us renderer accepted/errors=%" PRIu64 "/%" PRIu64 "\n",
        audio.mic_level,
        audio.last_capture_read_us,
        audio.afe_level,
        audio.last_fetch_us,
        audio.renderer_level,
        audio.last_renderer_accepted_us,
        audio.renderer_accepted,
        audio.renderer_errors);
    printf("wifi=%s ssid=\"%.32s\" rssi=%d dBm heap=%" PRIu32 "/%" PRIu32
           " B psram=%" PRIu32 " B\n",
        runtime.wifi_connected ? "connected" : "disconnected",
        runtime.wifi_ssid,
        runtime.wifi_rssi,
        runtime.internal_heap_free,
        runtime.internal_heap_largest,
        runtime.psram_free);
    printf("Talk VAD silence=%u ms player queues=%u/%u KiB\n",
        runtime.talk_vad_silence_ms,
        runtime.player_raw_queue_kib,
        runtime.player_render_queue_kib);
    return 0;
}

static int diagnostics_console_command(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "open") == 0) {
        return diagnostics_display_command(true);
    }
    if (argc == 2 && strcmp(argv[1], "close") == 0) {
        return diagnostics_display_command(false);
    }
    if (argc == 2 && strcmp(argv[1], "tone") == 0) {
        return diagnostics_tone_command();
    }
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        return diagnostics_status_command();
    }
    printf("usage: diagnostics open\n");
    printf("       diagnostics close\n");
    printf("       diagnostics tone\n");
    printf("       diagnostics status\n");
    return 1;
}

static esp_err_t register_room_console_commands(void)
{
    static const esp_console_cmd_t COMMANDS[] = {
        {
            .command = "wake",
            .help = "Simulate the wake word and start one Talk session",
            .func = wake_console_command,
        },
        {
            .command = "diagnostics",
            .help = "Control and inspect local room-node diagnostics",
            .hint = "open | close | tone | status",
            .func = diagnostics_console_command,
        },
    };
    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); ++i) {
        ESP_RETURN_ON_ERROR(
            esp_console_cmd_register(&COMMANDS[i]),
            TAG,
            "register local %s command",
            COMMANDS[i].command);
    }
    return ESP_OK;
}

bool esp_openclaw_room_node_try_acquire_camera(void)
{
    if (state_lock == NULL) return false;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool available = !camera_active && talk_call == NULL;
    if (available) camera_active = true;
    xSemaphoreGive(state_lock);
    return available;
}

void esp_openclaw_room_node_release_camera(void)
{
    if (state_lock == NULL) return;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    camera_active = false;
    xSemaphoreGive(state_lock);
}

esp_err_t esp_openclaw_room_node_camera_indicator_begin(void)
{
    return room_ui_camera_indicator_begin();
}

void esp_openclaw_room_node_camera_indicator_end(void)
{
    room_ui_camera_indicator_end();
}

static bool runtime_talk_busy(void *ctx)
{
    (void)ctx;
    if (state_lock == NULL) return false;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool busy = talk_call != NULL;
    xSemaphoreGive(state_lock);
    return busy;
}

esp_err_t room_runtime_request_test_tone(void)
{
    return room_media_request_test_tone(runtime_talk_busy, NULL);
}

void room_runtime_get_diagnostics(room_runtime_diagnostics_snapshot_t *snapshot)
{
    if (snapshot == NULL) return;
    memset(snapshot, 0, sizeof(*snapshot));
    if (state_lock != NULL) {
        xSemaphoreTake(state_lock, portMAX_DELAY);
        snapshot->node_ready = node_ready;
        snapshot->operator_ready = operator_ready;
        snapshot->media_ready = media_ready;
        snapshot->camera_active = camera_active;
        const char *phase = !media_ready ? "unavailable"
            : talk_closing ? "closing"
            : talk_active ? "active"
            : talk_dialing ? "dialing"
            : talk_start_in_flight ? "starting"
            : "idle";
        strlcpy(snapshot->talk_phase, phase, sizeof(snapshot->talk_phase));
        xSemaphoreGive(state_lock);
    } else {
        strlcpy(snapshot->talk_phase, "unavailable", sizeof(snapshot->talk_phase));
    }

    room_ui_diagnostics_snapshot_t ui = {0};
    room_ui_get_diagnostics(&ui);
    snapshot->diagnostics_open = ui.diagnostics_open;
    strlcpy(snapshot->ui_state, room_ui_state_name(ui.state), sizeof(snapshot->ui_state));
    strlcpy(snapshot->ui_detail, ui.detail, sizeof(snapshot->ui_detail));
    strlcpy(snapshot->gateway, ui.gateway, sizeof(snapshot->gateway));

    room_canvas_diagnostics_snapshot_t canvas = {0};
    room_canvas_get_diagnostics(&canvas);
    snapshot->canvas_active = canvas.active;
    snapshot->canvas_components = canvas.retained_components;
    snapshot->canvas_images = canvas.retained_images;
    const char *canvas_kind = canvas.retained_kind == ROOM_CANVAS_RETAINED_IMAGE ? "image"
        : canvas.retained_kind == ROOM_CANVAS_RETAINED_A2UI ? "a2ui"
        : "none";
    strlcpy(snapshot->canvas_kind, canvas_kind, sizeof(snapshot->canvas_kind));

    snapshot->talk_vad_silence_ms = ROOM_TALK_VAD_SILENCE_MS;
    snapshot->player_raw_queue_kib = ROOM_PLAYER_RAW_FIFO_KIB;
    snapshot->player_render_queue_kib = ROOM_PLAYER_RENDER_FIFO_KIB;
    const esp_openclaw_room_node_config_t *board = room_board_config();
    if (board != NULL) {
        strlcpy(snapshot->display_name,
            board->display_name != NULL ? board->display_name : "",
            sizeof(snapshot->display_name));
        strlcpy(snapshot->model_identifier,
            board->model_identifier != NULL ? board->model_identifier : "",
            sizeof(snapshot->model_identifier));
        snapshot->display_width = board->display.native_width;
        snapshot->display_height = board->display.native_height;
        strlcpy(snapshot->afe_layout,
            board->audio.afe_layout != NULL ? board->audio.afe_layout : "",
            sizeof(snapshot->afe_layout));
        snapshot->configured_volume = board->audio.playback_volume;
        snapshot->input_gain_configured = board->audio.configure_input_gain;
        snapshot->configured_input_gain_db = board->audio.input_gain_db;
    }
    esp_openclaw_node_wifi_status_t wifi = {0};
    esp_openclaw_node_wifi_get_status(&wifi);
    snapshot->wifi_connected = wifi.connected;
    strlcpy(snapshot->wifi_ssid, wifi.ssid, sizeof(snapshot->wifi_ssid));
    snapshot->wifi_rssi = wifi.rssi;
    strlcpy(snapshot->wifi_ip, wifi.ip, sizeof(snapshot->wifi_ip));
    snapshot->internal_heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    snapshot->internal_heap_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    snapshot->psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    snapshot->uptime_seconds = (uint64_t)esp_timer_get_time() / 1000000U;
}

esp_err_t esp_openclaw_room_node_start(const esp_openclaw_room_node_config_t *config)
{
    ESP_RETURN_ON_ERROR(room_board_bind(config), TAG, "invalid board contract");
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(media_lib_add_default_adapter());
    /* Default media stacks are insufficient for Opus/WebRTC; omitting these
     * schedulers causes a call-time stack failure. */
    ESP_ERROR_CHECK(
        esp_capture_set_thread_scheduler(capture_scheduler) == ESP_CAPTURE_ERR_OK
            ? ESP_OK
            : ESP_FAIL);
    media_lib_thread_set_schedule_cb(media_scheduler);
    if (config->services.prepare_runtime != NULL) {
        ESP_RETURN_ON_ERROR(
            config->services.prepare_runtime(config->services.ctx),
            TAG,
            "board runtime preparation");
    }

    state_lock = xSemaphoreCreateMutex();
    talk_teardown_queue = xQueueCreate(1, sizeof(talk_teardown_request_t));
    ESP_ERROR_CHECK(
        state_lock != NULL && talk_teardown_queue != NULL
            ? ESP_OK
            : ESP_ERR_NO_MEM);
    /* Supervisor stacks live in PSRAM; internal RAM is reserved for DMA and the
     * network/audio task stacks that genuinely require it. */
    BaseType_t starter_task = xTaskCreateWithCaps(
        talk_start_worker_task,
        "talk_start",
        8192,
        NULL,
        7,
        &talk_start_worker,
        MALLOC_CAP_SPIRAM);
    ESP_ERROR_CHECK(starter_task == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    BaseType_t teardown_task = xTaskCreate(
        talk_teardown_task,
        "talk_teardown",
        4096,
        NULL,
        6,
        NULL);
    ESP_ERROR_CHECK(teardown_task == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    room_ui_init();
    ESP_ERROR_CHECK(room_canvas_init());
    if (config->display.setup_local_input != NULL) {
        ESP_RETURN_ON_ERROR(
            config->display.setup_local_input(
                config->display.ctx,
                room_canvas_view_toggle),
            TAG,
            "board local input setup");
    }
    room_ui_set(ROOM_UI_CONNECTING, "Wi-Fi");
    if (config->services.prepare_network != NULL) {
        esp_err_t network_err = config->services.prepare_network(config->services.ctx);
        if (network_err != ESP_OK) {
            room_ui_set(ROOM_UI_ERROR, "Wi-Fi coprocessor unavailable");
            return network_err;
        }
    }
    ESP_ERROR_CHECK(esp_openclaw_node_wifi_start());
    ESP_LOGI(TAG, "Wi-Fi runtime started");
    seed_wifi_credentials_from_kconfig();
    esp_openclaw_node_wifi_status_t wifi_status = {0};
    esp_openclaw_node_wifi_get_status(&wifi_status);
    ESP_LOGI(
        TAG,
        "Wi-Fi state loaded: savedNetwork=%d connected=%d",
        wifi_status.has_saved_network,
        wifi_status.connected);
    if (wifi_status.has_saved_network &&
        !esp_openclaw_node_wifi_wait_for_connection(pdMS_TO_TICKS(30000))) {
        ESP_LOGW(TAG, "Wi-Fi did not connect within 30 s; fix credentials over the USB console");
    }

    ESP_LOGI(TAG, "initializing room media");
    esp_err_t media_err = room_media_init(on_wake, NULL);
    if (media_err != ESP_OK) {
        /* Canvas must stay usable when the unvalidated audio path fails; wake
         * and Talk stay off until the microphone path works. */
        ESP_LOGE(
            TAG,
            "room media init failed: %s; Talk wake is disabled",
            esp_err_to_name(media_err));
    } else {
        media_ready = true;
    }

    ESP_ERROR_CHECK(start_node_client());
    ESP_ERROR_CHECK(esp_openclaw_node_example_repl_start(node_client));
    ESP_ERROR_CHECK(register_room_console_commands());

    esp_err_t connect_err = request_node_connection();
    if (connect_err == ESP_ERR_NOT_FOUND) {
        room_ui_set(ROOM_UI_SETUP, "USB console:\nwifi set + setup-code");
        ESP_LOGI(
            TAG,
            "no saved session; provision over the USB console: wifi set <ssid> <passphrase>, then gateway setup-code <code>");
    } else if (connect_err != ESP_OK) {
        ESP_LOGE(TAG, "node connect request failed: %s", esp_err_to_name(connect_err));
        room_ui_set(ROOM_UI_ERROR, "Node connect");
    }
    ESP_LOGI(
        TAG,
        "room node ready; internal heap free=%u largest=%u, psram free=%u",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return ESP_OK;
}
