#include <string.h>

#include "esp_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_openclaw_node.h"
#include "esp_openclaw_talk.h"
#include "esp_peer.h"
#include "esp_peer_default.h"
#include "esp_wifi.h"
#include "esp_webrtc.h"
#include "esp_capture.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "nvs_flash.h"
#include "media_lib_adapter.h"
#include "media_lib_os.h"
#include "room_media.h"
#include "room_ui.h"

static const char *TAG = "openclaw_room";
static const EventBits_t WIFI_READY_BIT = BIT0;
static EventGroupHandle_t wifi_events;
static esp_openclaw_node_handle_t node_client;
static esp_openclaw_node_handle_t operator_client;
static esp_webrtc_handle_t webrtc;
static SemaphoreHandle_t state_lock;
static QueueHandle_t talk_teardown_queue;
static TimerHandle_t talk_timeout_timer;
static bool operator_ready;
static bool talk_starting;
static bool talk_closing;
static bool operator_start_scheduled;
static bool node_reconnect_scheduled;

typedef struct {
    esp_webrtc_handle_t session;
    bool failed;
} talk_teardown_request_t;

static void start_operator_task(void *arg);

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
    if (xTaskCreate(
            start_operator_task,
            "operator_start",
            6144,
            (void *)(uintptr_t)delay_ms,
            6,
            NULL) == pdPASS) {
        return true;
    }
    xSemaphoreTake(state_lock, portMAX_DELAY);
    operator_start_scheduled = false;
    xSemaphoreGive(state_lock);
    return false;
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

static void wifi_event(
    void *ctx,
    esp_event_base_t base,
    int32_t event_id,
    void *event_data)
{
    (void)ctx;
    (void)event_data;
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_events, WIFI_READY_BIT);
        esp_wifi_connect();
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_events, WIFI_READY_BIT);
    }
}

static esp_err_t connect_wifi(void)
{
    size_t ssid_len = strlen(CONFIG_OPENCLAW_ROOM_WIFI_SSID);
    size_t password_len = strlen(CONFIG_OPENCLAW_ROOM_WIFI_PASSWORD);
    wifi_config_t config = {0};
    if (ssid_len == 0 || ssid_len > sizeof(config.sta.ssid) ||
        password_len > sizeof(config.sta.password)) {
        return ESP_ERR_INVALID_ARG;
    }
    wifi_events = xEventGroupCreate();
    if (wifi_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "Wi-Fi init");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL),
        TAG,
        "Wi-Fi event handler");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL),
        TAG,
        "IP event handler");
    memcpy(config.sta.ssid, CONFIG_OPENCLAW_ROOM_WIFI_SSID, ssid_len);
    memcpy(config.sta.password, CONFIG_OPENCLAW_ROOM_WIFI_PASSWORD, password_len);
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Wi-Fi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &config), TAG, "Wi-Fi config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start");
    EventBits_t ready = xEventGroupWaitBits(
        wifi_events,
        WIFI_READY_BIT,
        pdFALSE,
        pdTRUE,
        pdMS_TO_TICKS(30000));
    return (ready & WIFI_READY_BIT) != 0 ? ESP_OK : ESP_ERR_TIMEOUT;
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
        if (current) {
            talk_starting = false;
        }
        xSemaphoreGive(state_lock);
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

static void start_talk_task(void *arg)
{
    (void)arg;
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
        vTaskDelete(NULL);
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
        vTaskDelete(NULL);
        return;
    }

    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool publish = talk_starting && !talk_closing && webrtc == NULL;
    if (publish) {
        webrtc = session;
    }
    xSemaphoreGive(state_lock);
    if (!publish) {
        esp_webrtc_close(session);
        vTaskDelete(NULL);
        return;
    }
    if (esp_webrtc_start(session) != 0) {
        request_talk_teardown(session, true);
        vTaskDelete(NULL);
        return;
    }
    if (xTimerStart(talk_timeout_timer, 0) != pdPASS) {
        request_talk_teardown(session, true);
    }
    vTaskDelete(NULL);
}

static void on_wake(const char *wake_word, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "ambient wake word detected: %s", wake_word);
    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool start = operator_ready && !talk_starting && !talk_closing && webrtc == NULL;
    if (start) {
        talk_starting = true;
    }
    xSemaphoreGive(state_lock);
    if (!start) {
        return;
    }
    room_ui_set(ROOM_UI_CONNECTING, wake_word);
    if (xTaskCreate(start_talk_task, "start_talk", 8192, NULL, 7, NULL) != pdPASS) {
        xSemaphoreTake(state_lock, portMAX_DELAY);
        talk_starting = false;
        xSemaphoreGive(state_lock);
        room_ui_set(ROOM_UI_ERROR, "No memory");
    }
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
    return esp_openclaw_node_request_connect(created, &request);
}

static void start_operator_task(void *arg)
{
    uint32_t delay_ms = (uint32_t)(uintptr_t)arg;
    if (delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
    // A synchronous/fast connect failure must be able to schedule the next retry.
    xSemaphoreTake(state_lock, portMAX_DELAY);
    operator_start_scheduled = false;
    xSemaphoreGive(state_lock);
    esp_err_t err = start_operator_client();
    if (err != ESP_OK) {
        room_ui_set(ROOM_UI_ERROR, "Operator token");
        (void)schedule_operator_start(5000);
    }
    vTaskDelete(NULL);
}

static esp_err_t request_node_connection(void)
{
    esp_openclaw_node_connect_request_t request = {0};
    if (esp_openclaw_node_has_saved_session(node_client)) {
        request.source = ESP_OPENCLAW_NODE_CONNECT_SOURCE_SAVED_SESSION;
    } else {
        if (CONFIG_OPENCLAW_ROOM_SETUP_CODE[0] == '\0') {
            return ESP_ERR_NOT_FOUND;
        }
        request.source = ESP_OPENCLAW_NODE_CONNECT_SOURCE_SETUP_CODE;
        request.value = CONFIG_OPENCLAW_ROOM_SETUP_CODE;
    }
    return esp_openclaw_node_request_connect(node_client, &request);
}

static void node_reconnect_task(void *arg)
{
    uint32_t delay_ms = (uint32_t)(uintptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    xSemaphoreTake(state_lock, portMAX_DELAY);
    node_reconnect_scheduled = false;
    xSemaphoreGive(state_lock);
    esp_err_t err = request_node_connection();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        xSemaphoreTake(state_lock, portMAX_DELAY);
        bool schedule = !node_reconnect_scheduled;
        node_reconnect_scheduled = true;
        xSemaphoreGive(state_lock);
        if (schedule &&
            xTaskCreate(
                node_reconnect_task,
                "node_reconnect",
                4096,
                (void *)(uintptr_t)5000,
                5,
                NULL) != pdPASS) {
            xSemaphoreTake(state_lock, portMAX_DELAY);
            node_reconnect_scheduled = false;
            xSemaphoreGive(state_lock);
        }
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
        if (!schedule_operator_start(0)) {
            room_ui_set(ROOM_UI_ERROR, "Operator token");
        }
    } else {
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
    ESP_RETURN_ON_ERROR(
        esp_openclaw_node_register_capability(node_client, "talk"),
        TAG,
        "talk capability");
    ESP_RETURN_ON_ERROR(
        esp_openclaw_node_register_capability(node_client, "voiceWake"),
        TAG,
        "wake capability");
    ESP_RETURN_ON_ERROR(
        esp_openclaw_node_register_capability(node_client, "display"),
        TAG,
        "display capability");

    return request_node_connection();
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
    BaseType_t teardown_task = xTaskCreate(
        talk_teardown_task,
        "talk_teardown",
        4096,
        NULL,
        6,
        NULL);
    ESP_ERROR_CHECK(teardown_task == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    room_ui_init();
    room_ui_set(ROOM_UI_CONNECTING, "Wi-Fi");
    ESP_ERROR_CHECK(connect_wifi());
    ESP_ERROR_CHECK(room_media_init(on_wake, NULL));
    ESP_ERROR_CHECK(start_node_client());
    ESP_LOGI(TAG, "room hardware ready; provision with `openclaw qr --voice-node`");
}
