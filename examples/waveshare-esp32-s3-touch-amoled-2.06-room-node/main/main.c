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
#include "driver/gpio.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "media_lib_adapter.h"
#include "media_lib_os.h"
#include "room_canvas.h"
#include "room_canvas_node_cmd.h"
#include "room_face.h"
#include "room_media.h"
#include "room_ui.h"

static const char *TAG = "openclaw_room";
static esp_openclaw_node_handle_t node_client;
static esp_openclaw_node_handle_t operator_client;
static esp_webrtc_handle_t webrtc;
static SemaphoreHandle_t state_lock;
static QueueHandle_t talk_teardown_queue;
static TimerHandle_t talk_timeout_timer;
static bool operator_ready;
static bool media_ready;
static bool talk_starting;
static bool talk_closing;
/* Set by talk.stop while the worker is still dialing; the worker must abandon
 * the session before the microphone path goes live. */
static bool talk_cancel_requested;
static bool node_reconnect_scheduled;

typedef struct {
    esp_webrtc_handle_t session;
    bool failed;
} talk_teardown_request_t;

static void start_talk_once(void);
static esp_err_t register_talk_node_commands(esp_openclaw_node_handle_t node);
static TaskHandle_t talk_start_worker;
/* One-shot timer that (re)tries creating the transient operator-start task.
 * A permanent worker task was tried and reverted: its always-resident internal
 * stack starved the AFE open/close sync-tasks during calls (internal RAM is
 * the scarcest resource on this board). Guarded by state_lock. */
static esp_timer_handle_t operator_start_timer;
static bool operator_start_scheduled;

/*
 * The Talk starter is a persistent worker created at boot, when internal RAM
 * is plentiful: creating an 8 KiB-stack task at wake time fails under load
 * and surfaced as an on-screen "No memory" error.
 */
/*
 * The BOOT side button (GPIO0, strapping pin, pressed = low) cycles the debug
 * views like a status-screen tap. The PWR button is wired to hardware power
 * management and is not GPIO-visible.
 */
static void boot_button_task(void *arg)
{
    (void)arg;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    if (gpio_config(&io) != ESP_OK) {
        vTaskDelete(NULL);
        return;
    }
    bool was_pressed = false;
    for (;;) {
        bool pressed = gpio_get_level(GPIO_NUM_0) == 0;
        if (pressed && !was_pressed) {
            room_canvas_view_toggle();
        }
        was_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(60));
    }
}

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

static char *load_saved_gateway_uri(void)
{
    nvs_handle_t nvs = 0;
    if (nvs_open("openclaw", NVS_READONLY, &nvs) != ESP_OK) {
        return NULL;
    }
    size_t required = 0;
    esp_err_t err = nvs_get_str(nvs, "session_uri", NULL, &required);
    char *uri = err == ESP_OK && required > 1 ? malloc(required) : NULL;
    if (uri != NULL && nvs_get_str(nvs, "session_uri", uri, &required) != ESP_OK) {
        free(uri);
        uri = NULL;
    }
    nvs_close(nvs);
    return uri;
}

static char *decode_setup_gateway_uri(void)
{
    const char *setup_code = CONFIG_OPENCLAW_ROOM_SETUP_CODE;
    size_t encoded_len = strlen(setup_code);
    if (encoded_len == 0) {
        return NULL;
    }
    size_t padded_len = ((encoded_len + 3) / 4) * 4;
    char *padded = calloc(padded_len + 1, 1);
    if (padded == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < encoded_len; ++i) {
        padded[i] = setup_code[i] == '-' ? '+' : setup_code[i] == '_' ? '/' : setup_code[i];
    }
    for (size_t i = encoded_len; i < padded_len; ++i) {
        padded[i] = '=';
    }
    size_t decoded_capacity = (padded_len / 4) * 3 + 1;
    unsigned char *decoded = calloc(decoded_capacity, 1);
    size_t decoded_len = 0;
    int rc = decoded != NULL
        ? mbedtls_base64_decode(
            decoded,
            decoded_capacity - 1,
            &decoded_len,
            (const unsigned char *)padded,
            padded_len)
        : -1;
    free(padded);
    if (rc != 0) {
        free(decoded);
        return NULL;
    }
    decoded[decoded_len] = '\0';
    cJSON *root = cJSON_Parse((const char *)decoded);
    free(decoded);
    cJSON *url = cJSON_IsObject(root)
        ? cJSON_GetObjectItemCaseSensitive(root, "url")
        : NULL;
    char *uri = cJSON_IsString(url) && url->valuestring != NULL
        ? strdup(url->valuestring)
        : NULL;
    cJSON_Delete(root);
    return uri;
}

static char *derive_gateway_http_base(void)
{
    if (CONFIG_OPENCLAW_ROOM_GATEWAY_HTTP_BASE_URL[0] != '\0') {
        return strdup(CONFIG_OPENCLAW_ROOM_GATEWAY_HTTP_BASE_URL);
    }
    char *gateway_uri = load_saved_gateway_uri();
    if (gateway_uri == NULL) {
        gateway_uri = decode_setup_gateway_uri();
    }
    char *base = gateway_http_base_from_uri(gateway_uri);
    free(gateway_uri);
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

static void media_thread_scheduler(const char *name, media_lib_thread_cfg_t *config)
{
    if (strcmp(name, "aenc_0") == 0 || strcmp(name, "AUD_SRC") == 0) {
        config->stack_size = 40 * 1024;
        config->priority = strcmp(name, "aenc_0") == 0 ? 10 : 15;
        config->core_id = 1;
    } else if (strcmp(name, "buffer_in") == 0) {
        config->stack_size = 6 * 1024;
        config->priority = 10;
        config->core_id = 0;
    } else if (strcmp(name, "pc_task") == 0) {
        config->stack_size = 25 * 1024;
        config->priority = 18;
        config->core_id = 1;
    } else if (strcmp(name, "pc_send") == 0) {
        config->stack_size = 4 * 1024;
        config->priority = 15;
        config->core_id = 1;
    } else if (strcmp(name, "Adec") == 0) {
        config->stack_size = 40 * 1024;
        config->priority = 15;
        config->core_id = 0;
    } else if (strcmp(name, "ARender") == 0) {
        config->priority = 20;
    }
}

static void capture_thread_scheduler(
    const char *name,
    esp_capture_thread_schedule_cfg_t *capture_config)
{
    media_lib_thread_cfg_t config = {
        .stack_size = capture_config->stack_size,
        .priority = capture_config->priority,
        .core_id = capture_config->core_id,
    };
    media_thread_scheduler(name, &config);
    capture_config->stack_size = config.stack_size;
    capture_config->priority = config.priority;
    capture_config->core_id = config.core_id;
    capture_config->stack_in_ext = true;
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
    if (status.configured) {
        return;
    }
    esp_err_t err = esp_openclaw_node_wifi_set_credentials(
        CONFIG_OPENCLAW_ROOM_WIFI_SSID,
        CONFIG_OPENCLAW_ROOM_WIFI_PASSWORD);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "seeding Wi-Fi credentials failed: %s", esp_err_to_name(err));
    }
}

static void request_talk_teardown(esp_webrtc_handle_t session, bool failed)
{
    talk_teardown_request_t request = {.session = session, .failed = failed};
    if (xQueueSend(talk_teardown_queue, &request, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Talk teardown already queued");
    }
}

static void talk_teardown_task(void *arg)
{
    (void)arg;
    for (;;) {
        talk_teardown_request_t request = {0};
        if (xQueueReceive(talk_teardown_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        xSemaphoreTake(state_lock, portMAX_DELAY);
        bool owns_session = webrtc == request.session && !talk_closing;
        if (owns_session) {
            talk_closing = true;
        }
        xSemaphoreGive(state_lock);
        if (!owns_session) {
            continue;
        }
        xTimerStop(talk_timeout_timer, portMAX_DELAY);
        esp_webrtc_close(request.session);
        (void)room_media_set_ambient_wake(true);
        room_ui_set(
            request.failed ? ROOM_UI_ERROR : ROOM_UI_IDLE,
            request.failed ? "Talk failed" : NULL);
        xSemaphoreTake(state_lock, portMAX_DELAY);
        if (webrtc == request.session) {
            webrtc = NULL;
            talk_starting = false;
        }
        talk_closing = false;
        xSemaphoreGive(state_lock);
    }
}

static void call_timeout(TimerHandle_t timer)
{
    (void)timer;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    esp_webrtc_handle_t session = webrtc;
    bool active = session != NULL && !talk_closing;
    xSemaphoreGive(state_lock);
    if (active) {
        request_talk_teardown(session, false);
    }
}

static int webrtc_event(esp_webrtc_event_t *event, void *ctx)
{
    esp_webrtc_handle_t session = ctx;
    if (event->type == ESP_WEBRTC_EVENT_CONNECTED) {
        xSemaphoreTake(state_lock, portMAX_DELAY);
        bool current = webrtc == session;
        bool cancelled = current && talk_cancel_requested;
        if (current) {
            talk_starting = false;
            talk_cancel_requested = false;
        }
        xSemaphoreGive(state_lock);
        if (cancelled) {
            /* A stop arrived while the connection was still coming up. */
            request_talk_teardown(session, false);
            return 0;
        }
        if (current) {
            room_ui_set(ROOM_UI_SPEAKING, NULL);
        }
    } else if (event->type == ESP_WEBRTC_EVENT_CONNECT_FAILED ||
               event->type == ESP_WEBRTC_EVENT_DISCONNECTED) {
        request_talk_teardown(
            session,
            event->type == ESP_WEBRTC_EVENT_CONNECT_FAILED);
    }
    return 0;
}

static void start_talk_once(void)
{
    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool cancelled_early = talk_cancel_requested;
    if (cancelled_early) {
        talk_starting = false;
        talk_cancel_requested = false;
    }
    xSemaphoreGive(state_lock);
    if (cancelled_early) {
        room_ui_set(ROOM_UI_IDLE, NULL);
        return;
    }
    esp_peer_default_cfg_t peer = {
        .agent_recv_timeout = 500,
        .ice_use_lite_mode = true,
    };
    esp_openclaw_talk_signaling_config_t signaling = {
        .operator_node = operator_client,
        .gateway_http_base_url = CONFIG_OPENCLAW_ROOM_GATEWAY_HTTP_BASE_URL,
        .session_key = "main",
        .provider = CONFIG_OPENCLAW_ROOM_TALK_PROVIDER,
        .model = CONFIG_OPENCLAW_ROOM_TALK_MODEL,
        .voice = CONFIG_OPENCLAW_ROOM_TALK_VOICE,
    };
    esp_webrtc_cfg_t config = {
        .peer_cfg = {
            .audio_info = {
                .codec = ESP_PEER_AUDIO_CODEC_OPUS,
                .sample_rate = 16000,
                .channel = 1,
            },
            .audio_dir = ESP_PEER_MEDIA_DIR_SEND_RECV,
            .enable_data_channel = true,
            .extra_cfg = &peer,
            .extra_size = sizeof(peer),
        },
        .signaling_cfg = {
            .extra_cfg = &signaling,
            .extra_size = sizeof(signaling),
        },
        .peer_impl = esp_peer_get_default_impl(),
        .signaling_impl = esp_openclaw_talk_signaling_impl(),
    };
    esp_webrtc_handle_t session = NULL;
    if (esp_webrtc_open(&config, &session) != 0) {
        xSemaphoreTake(state_lock, portMAX_DELAY);
        talk_starting = false;
        xSemaphoreGive(state_lock);
        room_ui_set(ROOM_UI_ERROR, "WebRTC open");
        return;
    }
    esp_webrtc_media_provider_t media = {0};
    if (room_media_get_webrtc_provider(&media) != ESP_OK ||
        esp_webrtc_set_media_provider(session, &media) != 0 ||
        esp_webrtc_set_no_auto_capture(session, true) != 0 ||
        esp_webrtc_set_event_handler(session, webrtc_event, session) != 0) {
        esp_webrtc_close(session);
        xSemaphoreTake(state_lock, portMAX_DELAY);
        talk_starting = false;
        xSemaphoreGive(state_lock);
        room_ui_set(ROOM_UI_ERROR, "WebRTC start");
        return;
    }

    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool cancelled = talk_cancel_requested;
    bool publish = !cancelled && talk_starting && !talk_closing && webrtc == NULL;
    if (publish) {
        webrtc = session;
    }
    if (cancelled) {
        talk_starting = false;
        talk_cancel_requested = false;
    }
    xSemaphoreGive(state_lock);
    if (!publish) {
        esp_webrtc_close(session);
        if (cancelled) {
            room_ui_set(ROOM_UI_IDLE, NULL);
        }
        return;
    }
    /* Talk owns the capture pipeline for the call; ambient scanning resumes at teardown. */
    (void)room_media_set_ambient_wake(false);
    if (esp_webrtc_start(session) != 0) {
        request_talk_teardown(session, true);
        return;
    }
    if (xTimerStart(talk_timeout_timer, 0) != pdPASS) {
        request_talk_teardown(session, true);
        return;
    }
    /* A stop that landed while esp_webrtc_start was in flight was recorded as
     * a cancel instead of a teardown so close never races start; honor it now
     * that start has returned. */
    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool cancel_after_start = talk_cancel_requested;
    talk_cancel_requested = false;
    xSemaphoreGive(state_lock);
    if (cancel_after_start) {
        request_talk_teardown(session, false);
    }
}

static void on_wake(const char *wake_word, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "ambient wake word detected: %s", wake_word);
    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool start = operator_ready && !talk_starting && !talk_closing && webrtc == NULL;
    if (start) {
        talk_starting = true;
        talk_cancel_requested = false;
    }
    xSemaphoreGive(state_lock);
    if (!start) {
        return;
    }
    room_ui_set(ROOM_UI_CONNECTING, wake_word);
    /* The magic beat: eyes snap wide the instant the wake word lands. */
    room_face_play_gesture(ROOM_FACE_GESTURE_SURPRISE);
    xTaskNotifyGive(talk_start_worker);
}

static void operator_gateway_event(
    esp_openclaw_node_handle_t client,
    const char *event,
    const char *payload_json,
    void *ctx)
{
    (void)client;
    (void)ctx;
    if (strcmp(event, "voicewake.changed") == 0 ||
        strcmp(event, "voicewake.routing.changed") == 0) {
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
    if (current) {
        operator_ready = event == ESP_OPENCLAW_NODE_EVENT_CONNECTED;
    }
    bool ready = current && operator_ready;
    xSemaphoreGive(state_lock);
    if (!current) {
        return;
    }
    if (ready) {
        room_ui_set(ROOM_UI_IDLE, NULL);
        /* Fully-up moment: greet with the face + gateway line for a few
         * seconds instead of silently going dark, then the hint expiry
         * returns the panel to idle. */
        room_ui_show_face_hint(8000);
        ESP_LOGI(TAG, "operator Talk session ready; ambient WakeNet armed");
    } else {
        room_ui_set(ROOM_UI_ERROR, "Operator offline");
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
    if (event == ESP_OPENCLAW_NODE_EVENT_CONNECTED) {
        /*
         * The session URI lands in NVS only after the first hello-ok, so the
         * boot-time base derivation is stale after console provisioning or a
         * re-pair to a different gateway. Refresh it on every connect.
         */
        char *gateway_http_base = derive_gateway_http_base();
        room_canvas_set_gateway_http_base(gateway_http_base);
        free(gateway_http_base);
        /* Show host:port under the face; the saved URI is authoritative for
         * which gateway this session actually authenticated against. */
        char *gateway_uri = load_saved_gateway_uri();
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
        room_ui_set(ROOM_UI_ERROR, "Node offline");
        schedule_node_reconnect(2000);
    }
}

static esp_err_t start_node_client(void)
{
    esp_openclaw_node_config_t config = {0};
    esp_openclaw_node_config_init_default(&config);
    config.display_name = "OpenClaw AMOLED Room Node";
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
    room_canvas_set_node(node_client);
    return ESP_OK;
}

/*
 * Bidirectional talk: the wake word starts a session from the room, and these
 * node commands let the agent start or end one remotely. Both directions run
 * the same client-owned WebRTC flow; the agent side simply plays the caller.
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
    (void)params_json;
    (void)params_len;
    if (!media_ready) {
        out_error->code = "UNAVAILABLE";
        out_error->message = "the microphone path failed at boot; talk is disabled on this node";
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool already_active = webrtc != NULL || talk_starting || talk_closing;
    bool start = operator_ready && !already_active;
    if (start) {
        talk_starting = true;
        talk_cancel_requested = false;
    }
    bool operator_offline = !operator_ready;
    xSemaphoreGive(state_lock);
    if (operator_offline) {
        out_error->code = "UNAVAILABLE";
        out_error->message = "the operator session is offline; the node reconnects automatically";
        return ESP_ERR_INVALID_STATE;
    }
    if (!start) {
        *out_payload_json = strdup("{\"started\":false,\"alreadyActive\":true}");
    } else {
        room_ui_set(ROOM_UI_CONNECTING, "agent");
        room_face_play_gesture(ROOM_FACE_GESTURE_SURPRISE);
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
    (void)params_json;
    (void)params_len;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    esp_webrtc_handle_t session = webrtc;
    /* While talk_starting is set, esp_webrtc_start may still be executing on
     * the worker; closing now would race it. Record a cancel instead — the
     * worker (or the CONNECTED event) performs the teardown once start has
     * returned, so close is always serialized behind start. */
    bool cancelling = talk_starting && !talk_closing;
    bool active = !cancelling && session != NULL && !talk_closing;
    if (cancelling) {
        talk_cancel_requested = true;
    }
    xSemaphoreGive(state_lock);
    if (active) {
        request_talk_teardown(session, false);
    }
    *out_payload_json = strdup(
        active || cancelling ? "{\"stopped\":true}" : "{\"stopped\":false}");
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

static esp_err_t register_wake_console_command(void)
{
    const esp_console_cmd_t command = {
        .command = "wake",
        .help = "Simulate the wake word and start one Talk session",
        .func = wake_console_command,
    };
    return esp_console_cmd_register(&command);
}

void app_main(void)
{
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(media_lib_add_default_adapter());
    ESP_ERROR_CHECK(
        esp_capture_set_thread_scheduler(capture_thread_scheduler) == ESP_CAPTURE_ERR_OK
            ? ESP_OK
            : ESP_FAIL);
    media_lib_thread_set_schedule_cb(media_thread_scheduler);

    state_lock = xSemaphoreCreateMutex();
    talk_teardown_queue = xQueueCreate(2, sizeof(talk_teardown_request_t));
    talk_timeout_timer = xTimerCreate(
        "talk_timeout",
        pdMS_TO_TICKS(CONFIG_OPENCLAW_ROOM_CALL_IDLE_SECONDS * 1000),
        pdFALSE,
        NULL,
        call_timeout);
    ESP_ERROR_CHECK(
        state_lock != NULL && talk_teardown_queue != NULL && talk_timeout_timer != NULL
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
    char *gateway_http_base = derive_gateway_http_base();
    room_canvas_set_gateway_http_base(gateway_http_base);
    free(gateway_http_base);
    room_ui_set(ROOM_UI_CONNECTING, "Wi-Fi");
    ESP_ERROR_CHECK(esp_openclaw_node_wifi_start());
    seed_wifi_credentials_from_kconfig();
    esp_openclaw_node_wifi_status_t wifi_status = {0};
    esp_openclaw_node_wifi_get_status(&wifi_status);
    if (wifi_status.configured &&
        !esp_openclaw_node_wifi_wait_for_connection(pdMS_TO_TICKS(30000))) {
        ESP_LOGW(TAG, "Wi-Fi did not connect within 30 s; fix credentials over the USB console");
    }

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
    if (xTaskCreateWithCaps(
            boot_button_task,
            "boot_btn",
            2560,
            NULL,
            4,
            NULL,
            MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGW(TAG, "BOOT button task unavailable");
    }
    ESP_ERROR_CHECK(register_wake_console_command());

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
}
