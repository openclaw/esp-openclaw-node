#include <inttypes.h>
#include <string.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>

#include "cJSON.h"
#include "esp_console.h"
#include "esp_event.h"
#include "esp_check.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_rom_sys.h"
#include "esp_openclaw_node.h"
#include "esp_openclaw_node_example_repl.h"
#include "esp_openclaw_node_wifi.h"
#include "esp_openclaw_talk.h"
#include "esp_peer.h"
#include "esp_peer_default.h"
#include "esp_timer.h"
#include "esp_wifi.h"
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
#include "room_agent_directory.h"
#include "room_messaging.h"
#include "esp_openclaw_devices.h"
#include "esp_openclaw_room_node.h"

static const char *TAG = "openclaw_room";
static esp_openclaw_node_handle_t node_client;
static esp_openclaw_node_handle_t operator_client;
static esp_webrtc_handle_t webrtc;
static SemaphoreHandle_t state_lock;
static QueueHandle_t talk_teardown_queue;
static TimerHandle_t talk_timeout_timer;
static portMUX_TYPE talk_timeout_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t talk_timeout_generation;
static bool operator_ready;
static bool node_ready;
static bool media_ready;
static bool media_initialized;
static bool node_session_missing;
static bool operator_session_missing;
static bool node_connection_pending;
static room_ui_wifi_state_t home_wifi_state;
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
static bool talk_speech_failed_pending;
static bool operator_ui_pending;
static bool voice_actions_pending;
static _Atomic uint64_t local_messaging_request = UINT64_MAX;
static _Atomic size_t local_agents_request = SIZE_MAX;
static _Atomic uint64_t local_devices_request = UINT64_MAX;
static unsigned published_device_generation;
static struct {
    bool selected, expired;
    char id[65], name[65], session_key[200];
    esp_openclaw_avatar_t avatar;
} talk_target;
/* State-lock owned: selection and invalidation revoke queued Start intents. */
static uint64_t talk_target_revision;
static portMUX_TYPE selection_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    bool pending;
    unsigned revision;
    char id[65];
} local_agent_selection;
static bool node_reconnect_scheduled;
static bool node_reconnect_pending;
static esp_timer_handle_t node_reconnect_timer;
static bool camera_active;
static char *gateway_http_base;
static struct {
    bool pending, start;
    uint32_t generation;
    uint64_t operator_incarnation;
    uint64_t recipient_revision;
} local_talk_request;

/* A one-slot wakeup has no identity or action to become stale. The durable,
 * generation-checked facts under state_lock are the only work authority. */
typedef uint8_t talk_teardown_request_t;

static int talk_stage(uint32_t generation, const char *stage, const char *phase, int result)
{
    ESP_LOGI(TAG, "room_talk_diag generation=%" PRIu32 " stage=%s phase=%s result=%d",
        generation, stage, phase, result);
    return result;
}

static void talk_audio_snapshot(uint32_t generation, const char *stage)
{
    room_audio_diagnostics_snapshot_t audio = {0};
    room_diagnostics_audio_get(&audio);
    ESP_LOGI(TAG,
        "room_talk_audio generation=%" PRIu32 " stage=%s capture_ok=%" PRIu64
        " capture_err=%" PRIu64 " feed_ok=%" PRIu64 " feed_err=%" PRIu64
        " fetch_ok=%" PRIu64 " fetch_err=%" PRIu64 " render_ok=%" PRIu64 " render_err=%" PRIu64
        " read_interval_max_us=%" PRIu64 " fetch_max_us=%" PRIu64,
        generation, stage, audio.capture_read_successes, audio.capture_read_errors,
        audio.feed_successes, audio.feed_errors, audio.fetch_successes, audio.fetch_errors,
        audio.renderer_accepted, audio.renderer_errors,
        audio.max_read_interval_us, audio.max_fetch_duration_us);
}

static void refresh_home_facts(void)
{
    xSemaphoreTake(state_lock, portMAX_DELAY);
    const room_ui_facts_t facts = {
        .wifi = home_wifi_state,
        .gateway = node_client == NULL ? ROOM_UI_GATEWAY_STARTING
            : node_ready ? ROOM_UI_GATEWAY_CONNECTED
            : node_session_missing ? ROOM_UI_GATEWAY_NO_SESSION
            : node_connection_pending ? ROOM_UI_GATEWAY_CONNECTING : ROOM_UI_GATEWAY_OFFLINE,
        .talk = !media_initialized ? ROOM_UI_TALK_STARTING
            : !media_ready ? ROOM_UI_TALK_UNAVAILABLE
            : talk_call != NULL && (talk_cancel_requested || talk_closing) ? ROOM_UI_TALK_STOPPING
            : talk_active ? ROOM_UI_TALK_ACTIVE
            : talk_call != NULL ? ROOM_UI_TALK_CONNECTING
            : operator_ready ? ROOM_UI_TALK_READY
            : operator_session_missing ? ROOM_UI_TALK_NO_SESSION : ROOM_UI_TALK_WAITING,
    };
    /* Store while the owner is locked, but never wait for LVGL under that lock. */
    room_ui_store_facts(&facts);
    room_ui_talk_target_t target = {.selected = talk_target.selected, .expired = talk_target.expired,
        .revision = talk_target_revision, .avatar = talk_target.avatar};
    memcpy(target.id, talk_target.id, sizeof(target.id));
    memcpy(target.name, talk_target.name, sizeof(target.name));
    room_ui_store_talk_target(&target);
    xSemaphoreGive(state_lock);
    room_ui_refresh();
}

static void refresh_wifi_facts(esp_event_base_t base, int32_t event_id)
{
    esp_openclaw_node_wifi_status_t wifi = {0};
    esp_openclaw_node_wifi_get_status(&wifi);
    bool offline = (base == WIFI_EVENT &&
        (event_id == WIFI_EVENT_STA_DISCONNECTED || event_id == WIFI_EVENT_STA_STOP)) ||
        (base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP);
    xSemaphoreTake(state_lock, portMAX_DELAY);
    home_wifi_state = wifi.connected && !offline ? ROOM_UI_WIFI_CONNECTED
        : !wifi.has_saved_network ? ROOM_UI_WIFI_UNCONFIGURED
        : offline ? ROOM_UI_WIFI_OFFLINE : ROOM_UI_WIFI_CONNECTING;
    xSemaphoreGive(state_lock);
    refresh_home_facts();
}

static void home_network_event(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    (void)arg;
    (void)data;
    if ((base == WIFI_EVENT && (event_id == WIFI_EVENT_STA_START ||
         event_id == WIFI_EVENT_STA_CONNECTED || event_id == WIFI_EVENT_STA_DISCONNECTED ||
         event_id == WIFI_EVENT_STA_STOP)) ||
        (base == IP_EVENT && (event_id == IP_EVENT_STA_GOT_IP || event_id == IP_EVENT_STA_LOST_IP))) {
        refresh_wifi_facts(base, event_id);
    }
}

static void media_scheduler(const char *name, media_lib_thread_cfg_t *cfg)
{
    /* The watch's encoder and capture fetch share core 1. Keep peer traffic
     * on core 0 so speech and networking leave time for display updates. */
    const esp_openclaw_room_node_config_t *board = room_board_config();
    bool watch = board != NULL && board->model_identifier != NULL &&
        strcmp(board->model_identifier, "waveshare-esp32-s3-touch-amoled-1.8") == 0;
    if (strcmp(name, "aenc_0") == 0 || strcmp(name, "AUD_SRC") == 0) {
        cfg->stack_size = 40 * 1024;
        cfg->priority = strcmp(name, "aenc_0") == 0 ? 10 : 15;
        cfg->core_id = 1;
    } else if (watch && strcmp(name, "aud_src") == 0) {
        /* Let the GMF source/copy pipeline share time with audio decoding. */
        cfg->priority = 15;
    } else if (strcmp(name, "buffer_in") == 0) {
        cfg->stack_size = 6 * 1024;
        cfg->priority = 10;
        cfg->core_id = 0;
    } else if (strcmp(name, "pc_task") == 0) {
        cfg->stack_size = 25 * 1024;
        cfg->priority = 18;
        cfg->core_id = watch ? 0 : 1;
    } else if (strcmp(name, "pc_send") == 0) {
        cfg->stack_size = 4 * 1024;
        cfg->priority = 15;
        cfg->core_id = watch ? 0 : 1;
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
static bool admit_talk_locked(void);
static esp_err_t register_talk_node_commands(esp_openclaw_node_handle_t node);
static TaskHandle_t talk_start_worker;
/* Timer and queued operator work are guarded by state_lock. The existing
 * internal-stack supervisor also performs NVS-backed operator startup. */
static esp_timer_handle_t operator_start_timer;
static bool operator_start_scheduled;
static bool operator_start_pending;

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

static void start_operator_if_pending(void);
static void start_node_if_pending(void);
static void node_reconnect_timer_fired(void *arg);
static void wake_talk_teardown(void);
static bool messaging_idle_locked(void);

/* The timer only hands work to the supervisor. A transient startup stack
 * competes with the SDK worker and WebSocket stacks at their allocation peak. */
static void operator_start_timer_fired(void *arg)
{
    (void)arg;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    operator_start_pending = true;
    xSemaphoreGive(state_lock);
    wake_talk_teardown();
}

static esp_err_t create_connection_timers(void)
{
    esp_timer_create_args_t args = {
        .callback = operator_start_timer_fired,
        .name = "operator_start",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &operator_start_timer), TAG, "operator retry timer");
    args.callback = node_reconnect_timer_fired;
    args.name = "node_reconnect";
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &node_reconnect_timer), TAG, "node retry timer");
    esp_err_t err = room_agent_directory_init(state_lock, wake_talk_teardown);
    room_messaging_init(state_lock, wake_talk_teardown);
    if (err != ESP_OK) ESP_LOGW(TAG, "agent directory unavailable: %s", esp_err_to_name(err));
    err = esp_openclaw_devices_init(state_lock, wake_talk_teardown);
    if (err != ESP_OK) ESP_LOGW(TAG, "device directory unavailable: %s", esp_err_to_name(err));
    return ESP_OK;
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
    esp_timer_stop(operator_start_timer);
    esp_err_t err = esp_timer_start_once(operator_start_timer, (uint64_t)delay_ms * 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "operator timer start failed: %s (%d)", esp_err_to_name(err), (int)err);
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

static void request_local_agents(bool open, size_t offset)
{
    atomic_store(&local_agents_request, open ? offset : SIZE_MAX - 1);
    wake_talk_teardown();
}

static void request_local_devices(bool open, unsigned generation)
{
    atomic_store(&local_devices_request, ((uint64_t)generation << 1) | (open ? 1u : 0u));
    wake_talk_teardown();
}

static void refresh_devices(void)
{
    uint64_t request = atomic_exchange(&local_devices_request, UINT64_MAX);
    if (request == UINT64_MAX) return;
    esp_openclaw_devices_cancel();
    published_device_generation = (unsigned)(request >> 1);
    if (request & 1u) (void)esp_openclaw_devices_refresh();
}

static void publish_devices(void)
{
    static esp_openclaw_devices_snapshot_t *snapshot;
    if (!room_ui_wants_devices()) {
        free(snapshot);
        snapshot = NULL;
        return;
    }
    if (snapshot == NULL) snapshot = malloc(sizeof(*snapshot));
    if (snapshot == NULL) { room_ui_devices_failed(published_device_generation); return; }
    esp_openclaw_devices_copy(snapshot);
    room_ui_store_devices(snapshot, published_device_generation);
}

static void invalidate_talk_target_locked(void)
{
    if (talk_target.selected && !talk_target.expired) {
        talk_target.expired = true;
        ++talk_target_revision;
    }
}

static esp_err_t select_talk_agent(const char *id, unsigned revision)
{
    room_agent_target_t target;
    char key[sizeof(talk_target.session_key)];
    xSemaphoreTake(state_lock, portMAX_DELAY);
    esp_err_t err = room_agent_directory_resolve_locked(revision, id, &target);
    if (err == ESP_OK) {
        int length = snprintf(key, sizeof(key), "agent:%s:%s", target.agent.id, target.main_key);
        if (length < 0 || (size_t)length >= sizeof(key)) err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK) {
        ++talk_target_revision;
        memcpy(talk_target.id, target.agent.id, sizeof(talk_target.id));
        memcpy(talk_target.name, target.agent.name, sizeof(talk_target.name));
        talk_target.avatar = esp_openclaw_agent_avatar(&target.agent);
        memcpy(talk_target.session_key, key, strlen(key) + 1);
        talk_target.selected = true;
        talk_target.expired = false;
    }
    xSemaphoreGive(state_lock);
    return err;
}

static void reset_talk_agent(void)
{
    xSemaphoreTake(state_lock, portMAX_DELAY);
    ++talk_target_revision;
    memset(&talk_target, 0, sizeof(talk_target));
    xSemaphoreGive(state_lock);
}

static void request_local_agent_selection(const char *id, unsigned revision)
{
    if (id == NULL || strnlen(id, sizeof(local_agent_selection.id)) >= sizeof(local_agent_selection.id)) return;
    portENTER_CRITICAL(&selection_lock);
    local_agent_selection.pending = true;
    local_agent_selection.revision = revision;
    memcpy(local_agent_selection.id, id, strlen(id) + 1);
    portEXIT_CRITICAL(&selection_lock);
    wake_talk_teardown();
}

static void apply_local_agent_selection(void)
{
    char id[sizeof(local_agent_selection.id)];
    portENTER_CRITICAL(&selection_lock);
    bool pending = local_agent_selection.pending;
    unsigned revision = local_agent_selection.revision;
    memcpy(id, local_agent_selection.id, sizeof(id));
    local_agent_selection.pending = false;
    portEXIT_CRITICAL(&selection_lock);
    if (pending) {
        esp_err_t result = ESP_OK;
        if (id[0] == '\0') reset_talk_agent();
        else result = select_talk_agent(id, revision);
        room_ui_agent_selection_complete(result);
    }
}

static struct {
    bool pending;
    room_ui_conversation_action_t action;
    char id[65];
    unsigned revision;
    size_t value;
    unsigned generation;
} local_conversation;
static atomic_uint published_conversation_generation;
static portMUX_TYPE conversation_lock = portMUX_INITIALIZER_UNLOCKED;
/* Supervisor-owned retries never overwrite a newer UI intent in the mailbox. */
static struct { bool pending; unsigned generation; } conversation_resume;

static void request_local_conversation(room_ui_conversation_action_t action, const char *id,
    unsigned revision, size_t value, unsigned generation)
{
    if (id == NULL || strnlen(id, sizeof(local_conversation.id)) >= sizeof(local_conversation.id)) return;
    portENTER_CRITICAL(&conversation_lock);
    local_conversation.pending = true;
    local_conversation.action = action;
    local_conversation.revision = revision;
    local_conversation.value = value;
    local_conversation.generation = generation;
    memcpy(local_conversation.id, id, strlen(id) + 1);
    portEXIT_CRITICAL(&conversation_lock);
    wake_talk_teardown();
}

static void apply_local_conversation(void)
{
    portENTER_CRITICAL(&conversation_lock);
    bool pending = local_conversation.pending;
    room_ui_conversation_action_t action = local_conversation.action;
    char id[sizeof(local_conversation.id)];
    memcpy(id, local_conversation.id, sizeof(id));
    unsigned revision = local_conversation.revision;
    size_t value = local_conversation.value;
    unsigned generation = local_conversation.generation;
    local_conversation.pending = false;
    portEXIT_CRITICAL(&conversation_lock);
    if (!pending) return;
    conversation_resume.pending = action == ROOM_CONVERSATIONS_RESUME;
    conversation_resume.generation = generation;
    if (conversation_resume.pending) return;
    esp_err_t err = ESP_OK;
    if (action == ROOM_CONVERSATIONS_OPEN) err = room_conversations_open(id, revision);
    else if (action == ROOM_CONVERSATIONS_LOAD) err = room_conversations_refresh(value);
    else if (action == ROOM_CONVERSATIONS_READ) err = room_conversations_preview(value, revision);
    else if (action == ROOM_CONVERSATIONS_NEW) err = room_conversations_creation_begin(revision);
    else if (action == ROOM_CONVERSATIONS_CREATE) err = room_conversations_creation_submit(revision);
    else if (action == ROOM_CONVERSATIONS_DISMISS_CREATION) room_conversations_creation_dismiss(revision);
    else if (action == ROOM_CONVERSATIONS_OPEN_CREATED) err = room_conversations_creation_open(revision);
    else if (action == ROOM_CONVERSATIONS_SUSPEND) room_agent_directory_suspend();
    else room_conversations_close();
    atomic_store(&published_conversation_generation, generation);
    bool creation = action == ROOM_CONVERSATIONS_NEW || action == ROOM_CONVERSATIONS_CREATE ||
        action == ROOM_CONVERSATIONS_DISMISS_CREATION || action == ROOM_CONVERSATIONS_OPEN_CREATED;
    if (err != ESP_OK && !creation) { room_conversations_close(); room_ui_conversations_failed(generation); }
}

static void publish_conversations(void)
{
    static room_conversations_snapshot_t *snapshot;
    room_conversation_context_t context;
    room_conversations_copy_context(&context);
    room_ui_store_conversation_context(&context);
    if (conversation_resume.pending) {
        unsigned generation = conversation_resume.generation;
        if (!room_ui_wants_conversation_resume(generation)) conversation_resume.pending = false;
        else {
            room_agents_snapshot_t *agents = malloc(sizeof(*agents));
            if (snapshot == NULL) snapshot = malloc(sizeof(*snapshot));
            unsigned epoch = 0;
            bool allocated = agents != NULL && snapshot != NULL;
            bool available = allocated &&
                room_conversations_copy_resume(agents, snapshot, &epoch) == ESP_OK;
            esp_err_t err = ESP_ERR_NO_MEM;
            if (allocated) err = room_ui_watch_resume(available ? agents : NULL,
                available ? snapshot : NULL, epoch, generation);
            else room_ui_conversations_failed(generation);
            free(agents);
            if (err == ESP_OK || !room_ui_wants_conversation_resume(generation)) {
                conversation_resume.pending = false;
                atomic_store(&published_conversation_generation, generation);
            }
        }
    }
    if (!room_ui_wants_conversations()) {
        free(snapshot);
        snapshot = NULL;
        return;
    }
    if (snapshot == NULL) snapshot = malloc(sizeof(*snapshot));
    if (snapshot == NULL) {
        room_ui_conversations_failed(atomic_load(&published_conversation_generation));
        return;
    }
    room_conversations_copy(snapshot);
    room_ui_store_conversations(snapshot, atomic_load(&published_conversation_generation));
}

static void refresh_agents(void)
{
    size_t request = atomic_exchange(&local_agents_request, SIZE_MAX);
    if (request == SIZE_MAX - 1) room_agent_directory_cancel();
    else if (request != SIZE_MAX) (void)room_agent_directory_refresh(request);
}

static void request_local_talk(bool start, uint64_t recipient_revision)
{
    /* The LVGL callback only records intent. The supervisor owns admission
     * and cancellation after the display lock has been released. */
    xSemaphoreTake(state_lock, portMAX_DELAY);
    local_talk_request.pending = true;
    local_talk_request.start = start;
    local_talk_request.generation = talk_generation;
    local_talk_request.operator_incarnation = operator_incarnation;
    local_talk_request.recipient_revision = recipient_revision;
    xSemaphoreGive(state_lock);
    wake_talk_teardown();
}

static void request_local_messaging(unsigned revision)
{
    atomic_store(&local_messaging_request, revision);
    wake_talk_teardown();
}

static void refresh_messaging(void)
{
    uint64_t revision = atomic_exchange(&local_messaging_request, UINT64_MAX);
    room_messaging_snapshot_t snapshot;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool idle = messaging_idle_locked();
    if (revision != UINT64_MAX) (void)room_messaging_request_locked((unsigned)revision, idle);
    room_messaging_copy_locked(&snapshot, idle);
    xSemaphoreGive(state_lock);
    room_ui_store_messaging(&snapshot);
}

static void request_local_voice(room_voice_action_t action, uint32_t generation, uint64_t revision, uint64_t owner)
{
    room_voice_draft_request(action, generation, revision, owner);
    wake_talk_teardown();
}

static void refresh_voice(void)
{
    xSemaphoreTake(state_lock, portMAX_DELAY);
    room_voice_context_t context = {
        .valid = node_ready && operator_ready && !talk_target.expired,
        .recipient_revision = talk_target_revision,
        .owner_incarnation = operator_incarnation,
        .operator_node = operator_client,
    };
    context.available = context.valid && media_ready && talk_call == NULL && !room_messaging_busy_locked();
    strlcpy(context.recipient, talk_target.selected ? talk_target.name : "Default agent", sizeof(context.recipient));
    if (talk_target.selected && !talk_target.expired) {
        strlcpy(context.agent_id, talk_target.id, sizeof(context.agent_id));
        strlcpy(context.session_key, talk_target.session_key, sizeof(context.session_key));
    }
    context.send_enabled = room_messaging_ready_locked();
    static room_voice_snapshot_t snapshot;
    bool changed = room_voice_draft_poll(&context, &snapshot);
    xSemaphoreGive(state_lock);
    if (changed) {
        room_ui_store_voice(&snapshot);
        room_ui_refresh();
    }
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
    bool accepted = request_talk_stop_locked(generation, message);
    xSemaphoreGive(state_lock);
    talk_stage(generation, "stop_request", accepted ? "accepted" : "ignored", 0);
}

static void talk_teardown_task(void *arg)
{
    (void)arg;
    for (;;) {
        talk_teardown_request_t wake;
        bool poll = conversation_resume.pending || room_voice_draft_needs_poll();
        xSemaphoreTake(state_lock, portMAX_DELAY);
        bool messaging_poll = room_messaging_busy_locked();
        xSemaphoreGive(state_lock);
        TickType_t wait = poll ? pdMS_TO_TICKS(100) : messaging_poll ? pdMS_TO_TICKS(1000) : portMAX_DELAY;
        if (xQueueReceive(talk_teardown_queue, &wake, wait) != pdTRUE && !poll && !messaging_poll) continue;
        /* Publish the target and repaint once below, after selection and Talk work. */
        apply_local_agent_selection();
        portENTER_CRITICAL(&talk_timeout_lock);
        uint32_t expired_generation = talk_timeout_generation;
        talk_timeout_generation = 0;
        portEXIT_CRITICAL(&talk_timeout_lock);
        xSemaphoreTake(state_lock, portMAX_DELAY);
        bool timeout_accepted = expired_generation != 0 &&
            request_talk_stop_locked(expired_generation, NULL);
        bool local_start = false;
        bool local_error = false;
        bool recipient_changed = false;
        bool target_expired = talk_target.selected && talk_target.expired;
        if (local_talk_request.pending) {
            bool current = local_talk_request.generation == talk_generation &&
                local_talk_request.operator_incarnation == operator_incarnation;
            if (current && local_talk_request.start) {
                recipient_changed = local_talk_request.recipient_revision != talk_target_revision;
                local_start = !recipient_changed && media_ready && admit_talk_locked();
                local_error = !local_start && talk_call == NULL;
            } else if (current) {
                request_talk_stop_locked(local_talk_request.generation, NULL);
            }
            local_talk_request.pending = false;
        }
        bool close = talk_call != NULL && talk_cancel_requested &&
            !talk_start_in_flight && !talk_closing;
        bool speaking = talk_speaking_pending && !talk_cancel_requested && !talk_closing;
        bool connecting = talk_connecting_pending && !talk_active && !talk_cancel_requested && !talk_closing;
        bool speech_failed = talk_speech_failed_pending && talk_active &&
            !talk_cancel_requested && !talk_closing;
        bool operator_ui = operator_ui_pending && talk_call == NULL;
        bool ready = operator_ready;
        talk_speaking_pending = false;
        talk_connecting_pending = false;
        talk_speech_failed_pending = false;
        operator_ui_pending = false;
        if (close) talk_closing = true;
        esp_webrtc_handle_t session = webrtc;
        esp_openclaw_talk_call_handle_t call = talk_call;
        const char *message = talk_cancel_message;
        uint32_t generation = talk_generation;
        xSemaphoreGive(state_lock);
        if (expired_generation != 0) {
            talk_stage(expired_generation, "stop_request", timeout_accepted ? "accepted" : "ignored", 0);
        }
        if (local_start) xTaskNotifyGive(talk_start_worker);
        refresh_home_facts();
        refresh_voice();
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
        if (local_error) room_ui_set(ROOM_UI_ERROR, recipient_changed ? "Recipient changed; start again" : target_expired
            ? "Choose an agent again after reconnecting" : "Talk unavailable; try again when ready");
        if (speech_failed) room_ui_set(ROOM_UI_ERROR, "Speech request failed; try again");
        if (!close) {
            start_node_if_pending();
            refresh_messaging();
            room_messaging_poll();
            refresh_messaging();
            start_operator_if_pending();
            xSemaphoreTake(state_lock, portMAX_DELAY);
            bool defer_directory = talk_start_in_flight || talk_dialing || talk_closing;
            xSemaphoreGive(state_lock);
            refresh_agents();
            refresh_devices();
            apply_local_conversation();
            room_agent_directory_poll(defer_directory);
            esp_openclaw_devices_poll(defer_directory);
            static room_agents_snapshot_t agents;
            room_agent_directory_copy(&agents);
            room_ui_store_agents(&agents);
            publish_conversations();
            publish_devices();
            continue;
        }

        /* The owner remains reserved until every local resource and UI update
         * is finished. Pending RPC refs may survive, but cannot dispatch. */
        talk_stage(generation, "stop_request", "consumed", 0);
        talk_audio_snapshot(generation, "teardown");
        talk_stage(generation, "quiesce", "begin", 0);
        esp_openclaw_talk_call_quiesce(call);
        talk_stage(generation, "quiesce", "end", 0);
        if (talk_timeout_timer != NULL) {
            xTimerStop(talk_timeout_timer, portMAX_DELAY);
            xTimerDelete(talk_timeout_timer, portMAX_DELAY);
            talk_timeout_timer = NULL;
        }
        if (session != NULL) {
            talk_stage(generation, "sdk_close", "begin", 0);
            int result = esp_webrtc_close(session);
            talk_stage(generation, "sdk_close", "end", result);
        }
        if (talk_ambient_suspended) {
            talk_stage(generation, "ambient_restore", "begin", 0);
            esp_err_t result = room_media_set_ambient_wake(true);
            talk_stage(generation, "ambient_restore", "end", result);
            if (result != ESP_OK) {
                ESP_LOGE(TAG, "failed to restore ambient WakeNet after Talk");
                message = "Talk failed";
            }
        }
        if (talk_media_owned) {
            talk_stage(generation, "media_release", "begin", 0);
            room_media_end_talk(!talk_ambient_suspended);
            talk_stage(generation, "media_release", "end", 0);
        }
        talk_audio_snapshot(generation, "released");
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
        refresh_home_facts();
        refresh_voice();
        start_node_if_pending();
        start_operator_if_pending();
        /* Teardown may have consumed the wake for a completed directory write.
         * Revisit deferred work after releasing the call and its media owner. */
        wake_talk_teardown();
    }
}

static void call_timeout(TimerHandle_t timer)
{
    /* The timer daemon must not block or format logs. Admissions never wrap
     * generations, so a delayed old expiry cannot overwrite a newer one. */
    uint32_t generation = (uint32_t)(uintptr_t)pvTimerGetTimerID(timer);
    portENTER_CRITICAL(&talk_timeout_lock);
    if (generation > talk_timeout_generation) talk_timeout_generation = generation;
    portEXIT_CRITICAL(&talk_timeout_lock);
    wake_talk_teardown();
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
    const char *stage = event->type == ESP_WEBRTC_EVENT_CONNECTING ? "peer_connecting" :
        event->type == ESP_WEBRTC_EVENT_PAIRED ? "peer_paired" :
        event->type == ESP_WEBRTC_EVENT_CONNECTED ? "peer_connected" :
        event->type == ESP_WEBRTC_EVENT_CONNECT_FAILED ? "peer_failed" :
        event->type == ESP_WEBRTC_EVENT_DISCONNECTED ? "peer_disconnected" : NULL;
    if (stage != NULL) {
        talk_stage(generation, stage, current ? "current" : "stale",
            (int)event->type);
        if (current) talk_audio_snapshot(generation, stage);
    }
    return 0;
}

/* Called under state_lock at wake/command admission, never in the worker. */
static bool admit_talk_locked(void)
{
    if (!operator_ready || operator_client == NULL || gateway_http_base == NULL ||
        camera_active || talk_call != NULL || room_voice_draft_busy() || room_messaging_busy_locked() ||
        talk_generation == UINT32_MAX) return false;
    if (talk_target.selected && talk_target.expired) return false;
    uint32_t generation = talk_generation + 1;
    esp_openclaw_talk_signaling_config_t signaling = {
        .operator_node = operator_client,
        .gateway_http_base_url = gateway_http_base,
        .provider = CONFIG_OPENCLAW_ROOM_TALK_PROVIDER,
        .model = CONFIG_OPENCLAW_ROOM_TALK_MODEL,
        .voice = CONFIG_OPENCLAW_ROOM_TALK_VOICE,
        .session_key = talk_target.selected ? talk_target.session_key : NULL,
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
    talk_speech_failed_pending = false;
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
    talk_stage(generation, "startup", "begin", 0);
    if (cancelled) goto done;
    talk_stage(generation, "media_acquire", "begin", 0);
    esp_err_t media_result = room_media_begin_talk();
    if (media_result != ESP_OK) {
        talk_stage(generation, "media_acquire", "error", media_result);
        request_talk_teardown(generation, "Audio is busy. Finish recording or the speaker test, then try again.");
        goto done;
    }
    talk_stage(generation, "media_acquire", "end", 0);
    talk_audio_snapshot(generation, "acquired");
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
    /* No-tag setters change global logging. Only lower a supported tag. */
#if CONFIG_LOG_DYNAMIC_LEVEL_CONTROL && !CONFIG_LOG_TAG_LEVEL_IMPL_NONE
    if (esp_log_level_get("webrtc") > ESP_LOG_WARN) {
        esp_log_level_set("webrtc", ESP_LOG_WARN);
    }
#endif
    bool sdk_log_safe = esp_log_level_get("webrtc") <= ESP_LOG_WARN;
#if !CONFIG_LOG_DYNAMIC_LEVEL_CONTROL
    /* Static logging uses the compiled maximum, not the getter's default. */
#ifdef CONFIG_LOG_MAXIMUM_LEVEL
    sdk_log_safe = sdk_log_safe && CONFIG_LOG_MAXIMUM_LEVEL < ESP_LOG_INFO;
#else
    sdk_log_safe = false;
#endif
#endif
    if (!sdk_log_safe) {
        talk_stage(generation, "sdk_log_policy", "end", ESP_FAIL);
        request_talk_teardown(generation, "Talk setup failed");
        goto done;
    }
    talk_stage(generation, "sdk_log_policy", "end", ESP_OK);
    talk_stage(generation, "sdk_open", "begin", 0);
    if (talk_stage(generation, "sdk_open", "end", esp_webrtc_open(&config, &session)) != 0) {
        request_talk_teardown(generation, "WebRTC open");
        goto done;
    }
    xSemaphoreTake(state_lock, portMAX_DELAY);
    webrtc = session;
    talk_dialing = true;
    xSemaphoreGive(state_lock);
    esp_webrtc_media_provider_t media = {0};
    if (talk_stage(generation, "media_provider", "end", room_media_get_webrtc_provider(&media)) != ESP_OK ||
        talk_stage(generation, "sdk_media_provider", "end", esp_webrtc_set_media_provider(session, &media)) != 0 ||
        talk_stage(generation, "sdk_capture_policy", "end", esp_webrtc_set_no_auto_capture(session, true)) != 0 ||
        talk_stage(generation, "sdk_event_handler", "end",
            esp_webrtc_set_event_handler(session, webrtc_event, (void *)(uintptr_t)generation)) != 0) {
        request_talk_teardown(generation, "WebRTC start");
        goto done;
    }
    xSemaphoreTake(state_lock, portMAX_DELAY);
    cancelled = talk_cancel_requested;
    if (!cancelled) talk_ambient_suspended = true;
    xSemaphoreGive(state_lock);
    if (cancelled) goto done;
    talk_stage(generation, "ambient_suspend", "begin", 0);
    if (talk_stage(generation, "ambient_suspend", "end", room_media_set_ambient_wake(false)) != ESP_OK) {
        request_talk_teardown(generation, "Talk setup failed");
        goto done;
    }
    talk_stage(generation, "sdk_start", "begin", 0);
    if (talk_stage(generation, "sdk_start", "end", esp_webrtc_start(session)) != 0 ||
        talk_stage(generation, "timer_start", "end", xTimerStart(talk_timeout_timer, 0)) != pdPASS) {
        request_talk_teardown(generation, "Talk setup failed");
    }

done:
    xSemaphoreTake(state_lock, portMAX_DELAY);
    talk_start_in_flight = false;
    if (talk_cancel_requested) wake_talk_teardown();
    cancelled = talk_cancel_requested;
    xSemaphoreGive(state_lock);
    talk_stage(generation, "startup", cancelled ? "canceled" : "end", 0);
}

static void on_wake(const char *wake_word, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "ambient wake word detected: %s", wake_word);
    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool start = admit_talk_locked();
    bool expired = talk_target.selected && talk_target.expired;
    xSemaphoreGive(state_lock);
    if (!start) {
        if (expired) room_ui_set(ROOM_UI_ERROR, "Choose an agent again after reconnecting");
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
    room_voice_draft_event(client, event, payload_json, talk_target_revision, operator_incarnation);
    esp_openclaw_talk_call_handle_t call = talk_call;
    bool bound = call != NULL && client == talk_operator && !talk_cancel_requested && !talk_closing &&
        (client != operator_client || talk_operator_incarnation == operator_incarnation);
    if (bound) esp_openclaw_talk_call_retain(call);
    xSemaphoreGive(state_lock);
    wake_talk_teardown();
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
    uint32_t connection = event == ESP_OPENCLAW_NODE_EVENT_CONNECTED
        ? esp_openclaw_node_get_connection_id(client) : 0;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool current = client == operator_client;
    bool connected = event == ESP_OPENCLAW_NODE_EVENT_CONNECTED;
    /* A displaced operator can still own the admitted call. Its loss cancels
     * that owner, but has no authority over the replacement operator's UI. */
    if (!connected && client == talk_operator) request_talk_stop_locked(talk_generation, NULL);
    if (current) {
        invalidate_talk_target_locked();
        operator_ready = connected;
        if (connected) {
            operator_session_missing = false;
            ++operator_incarnation;
            if (client == talk_operator && talk_operator_incarnation != operator_incarnation) {
                request_talk_stop_locked(talk_generation, NULL);
            }
        }
    }
    bool ready = current && operator_ready;
    if (current) {
        room_messaging_set_operator_locked(client, operator_incarnation, connection, connected);
        room_agent_directory_set_owner_locked(client, operator_incarnation, connection, connected);
        esp_openclaw_devices_set_owner_locked(client, operator_incarnation, connection, connected);
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

static void record_operator_session_result(esp_err_t err)
{
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) return;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    operator_session_missing = err == ESP_ERR_NOT_FOUND;
    xSemaphoreGive(state_lock);
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
        record_operator_session_result(err);
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
#if CONFIG_OPENCLAW_ROOM_VOICE_ACTIONS
    if (err == ESP_OK) {
        err = esp_openclaw_node_register_scope(created, "operator.write");
    }
    if (err == ESP_OK) {
        err = esp_openclaw_node_register_scope(created, "operator.admin");
    }
#endif
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
    err = esp_openclaw_node_request_connect(created, &request);
    record_operator_session_result(err);
    return err;
}

static void start_operator_if_pending(void)
{
    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool start = operator_start_pending && talk_call == NULL && !room_voice_draft_busy() &&
        !room_messaging_busy_locked();
    if (start) {
        operator_start_pending = false;
        operator_start_scheduled = false;
    }
    xSemaphoreGive(state_lock);
    if (!start) return;
    esp_err_t err = start_operator_client();
    refresh_home_facts();
    if (err != ESP_OK) {
        // Every retry logs: the display alone cannot say WHY the operator
        // session is down, and a silent loop here cost a debugging session.
        ESP_LOGW(TAG, "operator client start failed: %s (retrying)", esp_err_to_name(err));
        room_ui_set(
            ROOM_UI_ERROR,
            err == ESP_ERR_NOT_FOUND ? "No operator session\nre-pair via console" : "Operator token");
        (void)schedule_operator_start(5000);
    }
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
    if (err == ESP_ERR_NOT_FOUND && CONFIG_OPENCLAW_ROOM_SETUP_CODE[0] != '\0') {
        request.source = ESP_OPENCLAW_NODE_CONNECT_SOURCE_SETUP_CODE;
        request.value = CONFIG_OPENCLAW_ROOM_SETUP_CODE;
        err = esp_openclaw_node_request_connect(node_client, &request);
    }
    xSemaphoreTake(state_lock, portMAX_DELAY);
    if (err == ESP_OK || err == ESP_ERR_NOT_FOUND) node_session_missing = err == ESP_ERR_NOT_FOUND;
    node_connection_pending = err == ESP_OK;
    xSemaphoreGive(state_lock);
    refresh_home_facts();
    return err;
}

static void schedule_node_reconnect(uint32_t delay_ms);

static void node_reconnect_timer_fired(void *arg)
{
    (void)arg;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    node_reconnect_pending = true;
    xSemaphoreGive(state_lock);
    wake_talk_teardown();
}

static void start_node_if_pending(void)
{
    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool start = node_reconnect_pending && !node_ready;
    if (node_reconnect_pending) {
        node_reconnect_pending = false;
        node_reconnect_scheduled = false;
    }
    xSemaphoreGive(state_lock);
    if (!start) return;
    esp_err_t err = request_node_connection();
    if (err == ESP_ERR_NOT_FOUND) {
        /* A missing session requires provisioning, not an endless retry.
         * Node loss alone must not replace an active operator call's UI. */
        if (!room_ui_talk_face_active()) {
            room_ui_set(ROOM_UI_SETUP, "USB console:\ngateway setup-code");
        }
        return;
    }
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        schedule_node_reconnect(5000);
    }
}

static void schedule_node_reconnect(uint32_t delay_ms)
{
    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool schedule = !node_reconnect_scheduled;
    if (schedule) node_reconnect_scheduled = true;
    xSemaphoreGive(state_lock);
    if (!schedule) return;
    esp_timer_stop(node_reconnect_timer);
    esp_err_t err = esp_timer_start_once(node_reconnect_timer, (uint64_t)delay_ms * 1000);
    if (err != ESP_OK) {
        xSemaphoreTake(state_lock, portMAX_DELAY);
        node_reconnect_scheduled = false;
        xSemaphoreGive(state_lock);
        ESP_LOGE(TAG, "node retry timer start failed: %s (%d)", esp_err_to_name(err), (int)err);
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
    node_connection_pending = false;
    if (node_ready) node_session_missing = false;
    xSemaphoreGive(state_lock);
    refresh_home_facts();
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
        if (origin_changed) invalidate_talk_target_locked();
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
                room_messaging_invalidate_locked();
                room_messaging_set_operator_locked(existing_operator, operator_incarnation, 0, false);
                invalidate_talk_target_locked();
                room_agent_directory_set_owner_locked(existing_operator, operator_incarnation, 0, false);
                esp_openclaw_devices_set_owner_locked(existing_operator, operator_incarnation, 0, false);
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
        refresh_home_facts();
        free(gateway_uri);
        /* Retry timer arming through the same path. */
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
    bool voice_busy = room_voice_draft_busy();
    bool messaging_busy = room_messaging_busy_locked();
    bool start = admit_talk_locked();
    bool operator_offline = !operator_ready;
    bool target_expired = talk_target.selected && talk_target.expired;
    xSemaphoreGive(state_lock);
    if (operator_offline) {
        out_error->code = "UNAVAILABLE";
        out_error->message = "the operator session is offline; the node reconnects automatically";
        return ESP_ERR_INVALID_STATE;
    }
    if (!start) {
        if (messaging_busy) {
            out_error->code = "UNAVAILABLE";
            out_error->message = "finish the messaging permission request before starting Talk";
            return ESP_ERR_INVALID_STATE;
        }
        if (voice_busy) {
            out_error->code = "UNAVAILABLE";
            out_error->message = "finish or discard the recording before starting Talk";
            return ESP_ERR_INVALID_STATE;
        }
        if (target_expired && !already_active) {
            out_error->code = "UNAVAILABLE";
            out_error->message = "choose an agent again after reconnecting, or explicitly use the Gateway default";
            return ESP_ERR_INVALID_STATE;
        }
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

static void talk_say_result(
    esp_openclaw_node_handle_t node,
    const esp_openclaw_node_gateway_result_t *result, void *ctx)
{
    uint32_t generation = (uint32_t)(uintptr_t)ctx;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool current = generation == talk_generation && node == talk_operator &&
        operator_incarnation == talk_operator_incarnation && talk_active &&
        !talk_cancel_requested && !talk_closing;
    if (current && !result->ok) {
        talk_speech_failed_pending = true;
        wake_talk_teardown();
    }
    xSemaphoreGive(state_lock);
}

static esp_err_t handle_talk_say(
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
    cJSON *text = cJSON_GetObjectItemCaseSensitive(params, "text");
    bool valid = cJSON_IsObject(params) && params->child == text &&
        text != NULL && text->next == NULL && cJSON_IsString(text) &&
        text->valuestring[0] != '\0' &&
        strlen(text->valuestring) <= ESP_OPENCLAW_TALK_MAX_SPEECH_BYTES &&
        strstr(params_json, "\\u0000") == NULL;
    if (!valid) {
        cJSON_Delete(params);
        out_error->code = "INVALID_PARAMS";
        out_error->message = "talk.say requires only text (1 to 1024 UTF-8 bytes)";
        return ESP_ERR_INVALID_ARG;
    }
    char *payload = strdup("{\"queued\":true}");
    if (payload == NULL) {
        cJSON_Delete(params);
        out_error->code = "INTERNAL";
        out_error->message = "not enough memory for the command result";
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool ready = talk_active && operator_ready && talk_operator == operator_client &&
        talk_operator_incarnation == operator_incarnation &&
        !talk_cancel_requested && !talk_closing;
    esp_openclaw_talk_call_handle_t call = ready ? talk_call : NULL;
    uint32_t generation = talk_generation;
    if (call != NULL) esp_openclaw_talk_call_retain(call);
    xSemaphoreGive(state_lock);
    esp_err_t err = call != NULL
        ? esp_openclaw_talk_call_say(call, text->valuestring, talk_say_result,
            (void *)(uintptr_t)generation)
        : ESP_ERR_INVALID_STATE;
    esp_openclaw_talk_call_release(call);
    cJSON_Delete(params);
    if (err != ESP_OK) {
        free(payload);
        out_error->code = "UNAVAILABLE";
        out_error->message = "Talk must be connected with no speech request pending";
        return err;
    }
    *out_payload_json = payload;
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
    uint32_t generation = talk_generation;
    bool active = request_talk_stop_locked(generation, NULL);
    xSemaphoreGive(state_lock);
    talk_stage(generation, "stop_request", active ? "accepted" : "ignored", 0);
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
        {.name = "talk.say", .handler = handle_talk_say},
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

typedef struct {
    uint64_t incarnation;
    char *gateway_uri;
    char *request_id;
} voice_actions_request_t;

static void voice_actions_request_free(voice_actions_request_t *request)
{
    free(request->gateway_uri);
    free(request->request_id);
    free(request);
}

static void voice_actions_reply(
    esp_openclaw_node_handle_t client,
    const esp_openclaw_node_gateway_result_t *result,
    void *ctx)
{
    voice_actions_request_t *request = ctx;
    char *gateway_uri = esp_openclaw_node_dup_gateway_uri(client);
    xSemaphoreTake(state_lock, portMAX_DELAY);
    voice_actions_pending = false;
    bool current = client == operator_client && operator_ready &&
        operator_incarnation == request->incarnation && talk_call == NULL;
    xSemaphoreGive(state_lock);
    current = current && gateway_uri != NULL && strcmp(gateway_uri, request->gateway_uri) == 0;
    free(gateway_uri);
    cJSON *payload = result != NULL && result->ok && result->payload_json != NULL
        ? cJSON_Parse(result->payload_json) : NULL;
    cJSON *id = cJSON_GetObjectItemCaseSensitive(payload, "requestId");
    if (!current) {
        printf("voice-actions canceled: operator connection or Talk state changed; retry from idle\n");
    } else if (!cJSON_IsString(id) || id->valuestring[0] == '\0' || strlen(id->valuestring) > 128) {
        printf("voice-actions failed: check Gateway support and connection, then retry\n");
    } else if (request->request_id == NULL) {
        printf("voice-actions approval request: %s\n", id->valuestring);
        printf("approve this device's read/talk/write/admin owner-access request on the Gateway, then run voice-actions complete <requestId>\n");
    } else {
        cJSON *status = cJSON_GetObjectItemCaseSensitive(payload, "status");
        cJSON *token = cJSON_GetObjectItemCaseSensitive(payload, "deviceToken");
        cJSON *scopes = cJSON_GetObjectItemCaseSensitive(payload, "scopes");
        if (!cJSON_IsArray(scopes)) scopes = NULL;
        bool read = false, talk = false, write = false, admin = false;
        cJSON *scope;
        cJSON_ArrayForEach(scope, scopes) {
            if (!cJSON_IsString(scope)) continue;
            read |= strcmp(scope->valuestring, "operator.read") == 0;
            talk |= strcmp(scope->valuestring, "operator.talk") == 0;
            write |= strcmp(scope->valuestring, "operator.write") == 0;
            admin |= strcmp(scope->valuestring, "operator.admin") == 0;
        }
        if (strcmp(id->valuestring, request->request_id) != 0 ||
            !cJSON_IsString(status) || strcmp(status->valuestring, "approved") != 0 ||
            !cJSON_IsString(token) || token->valuestring[0] == '\0' || !read || !talk || !write || !admin) {
            printf("voice-actions not approved with read/talk/write/admin owner access; check the exact Gateway request\n");
        } else {
            /* The existing connect lifecycle validates and persists the replacement
             * token before READY. Never print it or introduce a second token store. */
            const esp_openclaw_node_connect_request_t connect = {
                .source = ESP_OPENCLAW_NODE_CONNECT_SOURCE_DEVICE_TOKEN,
                .gateway_uri = request->gateway_uri,
                .value = token->valuestring,
            };
            esp_err_t err = esp_openclaw_node_request_connect(client, &connect);
            printf("%s\n", err == ESP_OK
                ? "voice-actions credential reconnect queued; wait for operator Talk session ready before flashing the enabled build"
                : "voice-actions credential reconnect failed to queue; retry complete before restarting");
        }
    }
    cJSON_Delete(payload);
    voice_actions_request_free(request);
}

static int voice_actions_console_command(int argc, char **argv)
{
    bool complete = argc == 3 && strcmp(argv[1], "complete") == 0 &&
        argv[2][0] != '\0' && strlen(argv[2]) <= 128;
    if (!complete && !(argc == 2 && strcmp(argv[1], "request") == 0)) {
        printf("usage: voice-actions request | voice-actions complete <requestId>\n");
        return 1;
    }
    voice_actions_request_t *request = calloc(1, sizeof(*request));
    if (request == NULL) return 1;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    esp_openclaw_node_handle_t client = operator_client;
    bool ready = client != NULL && operator_ready && talk_call == NULL &&
        !room_messaging_busy_locked() && !voice_actions_pending &&
        !room_voice_draft_connection_busy() && !room_conversations_creation_busy_locked();
    if (ready) voice_actions_pending = true;
    request->incarnation = operator_incarnation;
    xSemaphoreGive(state_lock);
    request->gateway_uri = ready ? esp_openclaw_node_dup_gateway_uri(client) : NULL;
    request->request_id = complete ? strdup(argv[2]) : NULL;
    char *json = NULL;
    if (complete) {
        cJSON *params = cJSON_CreateObject();
        if (request->request_id != NULL &&
            cJSON_AddStringToObject(params, "requestId", request->request_id) != NULL) {
            json = cJSON_PrintUnformatted(params);
        }
        cJSON_Delete(params);
    } else {
        json = strdup("{\"scopes\":[\"operator.read\",\"operator.talk\",\"operator.write\",\"operator.admin\"]}");
    }
    esp_err_t err = request->gateway_uri != NULL && json != NULL
        ? esp_openclaw_node_gateway_request(client,
            complete ? "device.scopes.waitUpgrade" : "device.scopes.requestUpgrade",
            json, voice_actions_reply, request)
        : ESP_ERR_INVALID_STATE;
    free(json);
    if (err != ESP_OK) {
        xSemaphoreTake(state_lock, portMAX_DELAY);
        if (ready) voice_actions_pending = false;
        xSemaphoreGive(state_lock);
        printf("voice-actions unavailable: wait for an idle, connected operator session and retry\n");
        voice_actions_request_free(request);
        return 1;
    }
    printf("voice-actions %s queued\n", complete ? "completion" : "approval request");
    return 0;
}

static bool messaging_idle_locked(void)
{
    return talk_call == NULL && !camera_active && !voice_actions_pending &&
        !room_voice_draft_connection_busy() && !room_conversations_creation_busy_locked();
}

static int messaging_console_command(int argc, char **argv)
{
    bool request = argc == 2 && strcmp(argv[1], "request") == 0;
    if (!request && argc != 1) {
        printf("usage: messaging [request]\n");
        return 1;
    }
    room_messaging_snapshot_t snapshot;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool idle = messaging_idle_locked();
    room_messaging_copy_locked(&snapshot, idle);
    esp_err_t err = request ? room_messaging_request_locked(snapshot.revision, idle) : ESP_OK;
    room_messaging_copy_locked(&snapshot, idle);
    xSemaphoreGive(state_lock);
    const char *states[] = {"access-required", "requesting", "waiting-for-approval", "connecting",
        "enabled", "rejected", "expired", "error"};
    printf("messaging=%s connected=%s can-request=%s request-id=%s\n", states[snapshot.state],
        snapshot.connected ? "yes" : "no", snapshot.can_request ? "yes" : "no", snapshot.request_id);
    if (err != ESP_OK) printf("finish Talk, drafts, and pending chats; wait for the operator, then retry\n");
    return err == ESP_OK ? 0 : 1;
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

    char wifi_rssi[16] = "unknown";
    if (runtime.wifi_ap_info_valid) {
        snprintf(wifi_rssi, sizeof(wifi_rssi), "%d dBm", runtime.wifi_rssi);
    }
    printf("diagnostics=%s Talk=%s tone=%s%s%s (%u/%u/%u requested/queued/accepted)\n",
        runtime.diagnostics_open ? "open" : "closed",
        runtime.talk_phase,
        room_media_tone_state_name(tone.state),
        tone.state == ROOM_MEDIA_TONE_ERROR ? "/" : "",
        tone.state == ROOM_MEDIA_TONE_ERROR ? room_media_tone_error_name(tone.error) : "",
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
    printf("AFE feed accepted/errors=%" PRIu64 "/%" PRIu64
           " ring_free=%u%% read_interval_max_us=%" PRIu64 " fetch_max_us=%" PRIu64 "\n",
        audio.feed_successes, audio.feed_errors, audio.ringbuffer_free_percent,
        audio.max_read_interval_us, audio.max_fetch_duration_us);
    printf("wifi=%s ssid=\"%.32s\" rssi=%s heap=%" PRIu32 "/%" PRIu32
           " B psram=%" PRIu32 " B\n",
        runtime.wifi_connected ? "connected" : "disconnected",
        runtime.wifi_ssid,
        wifi_rssi,
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

static int devices_console_command(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "refresh") == 0)
        return esp_openclaw_devices_refresh() == ESP_OK ? 0 : 1;
    if (argc == 2 && strcmp(argv[1], "close") == 0) {
        esp_openclaw_devices_cancel();
        return 0;
    }
    if (argc != 1) { printf("usage: devices [refresh|close]\n"); return 1; }
    esp_openclaw_devices_snapshot_t *snapshot = malloc(sizeof(*snapshot));
    if (snapshot == NULL) return 1;
    esp_openclaw_devices_copy(snapshot);
    cJSON *root = cJSON_CreateObject();
    cJSON *nodes = root != NULL ? cJSON_AddArrayToObject(root, "nodes") : NULL;
    static const char *states[] = {"idle", "offline", "loading", "ready", "empty", "error", "too-large", "timeout"};
    bool ok = nodes != NULL && cJSON_AddStringToObject(root, "state", states[snapshot->state]) != NULL &&
        cJSON_AddNumberToObject(root, "revision", snapshot->revision) != NULL;
    for (size_t i = 0; ok && i < snapshot->count; ++i) {
        const esp_openclaw_device_t *device = &snapshot->devices[i];
        cJSON *row = cJSON_CreateObject();
        ok = row != NULL && cJSON_AddItemToArray(nodes, row);
        if (!ok) { cJSON_Delete(row); break; }
        ok = cJSON_AddStringToObject(row, "nodeId", device->id) != NULL &&
            cJSON_AddStringToObject(row, "displayName", device->name) != NULL &&
            cJSON_AddStringToObject(row, "modelIdentifier", device->model) != NULL &&
            (device->connected_known ? cJSON_AddBoolToObject(row, "connected", device->connected) :
                cJSON_AddNullToObject(row, "connected")) != NULL &&
            (device->paired_known ? cJSON_AddBoolToObject(row, "paired", device->paired) :
                cJSON_AddNullToObject(row, "paired")) != NULL;
    }
    char *json = ok ? cJSON_PrintUnformatted(root) : NULL;
    ok = json != NULL;
    if (ok) printf("devices=%s\n", json);
    free(snapshot);
    free(json);
    cJSON_Delete(root);
    return ok ? 0 : 1;
}

static int brightness_console_command(int argc, char **argv)
{
    char *end = NULL;
    long percent = argc == 2 ? strtol(argv[1], &end, 10) : -1;
    if (argc != 2 || end == argv[1] || *end != '\0' || percent < 10 || percent > 100) {
        printf("usage: brightness <10..100>\n");
        return 1;
    }
    esp_err_t err = room_ui_watch_brightness((int)percent);
    printf("watch brightness: %s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int menu_console_command(int argc, char **argv)
{
    if (argc > 2) {
        printf("usage: menu [menu|agents|conversations|preview|devices|device|settings|display|brightness|reading|density|messaging|connection|canvas|home]\n");
        return 1;
    }
    const char *page = argc == 2 ? argv[1] : "menu";
    esp_err_t err;
    if (strcmp(page, "preview") == 0) {
        room_conversations_snapshot_t *snapshot = malloc(sizeof(*snapshot));
        if (snapshot == NULL) return 1;
        room_conversations_copy(snapshot);
        err = room_ui_watch_reader(snapshot, strcmp(page, "preview") == 0,
            atomic_load(&published_conversation_generation));
        free(snapshot);
    } else err = room_ui_watch_menu(page);
    printf("watch menu: %s\n", err == ESP_OK ? page : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int agents_console_command(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "select") == 0) {
        room_agents_snapshot_t *snapshot = malloc(sizeof(*snapshot));
        if (snapshot == NULL) return 1;
        room_agent_directory_copy(snapshot);
        esp_err_t err = select_talk_agent(argv[2], snapshot->revision);
        free(snapshot);
        refresh_home_facts();
        printf("next Talk agent: %s\n", err == ESP_OK ? argv[2] : "selection unavailable; refresh agents and retry");
        return err == ESP_OK ? 0 : 1;
    }
    if (argc == 2 && strcmp(argv[1], "default") == 0) {
        reset_talk_agent();
        refresh_home_facts();
        printf("next Talk uses the Gateway default; current call is unchanged\n");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "selection") == 0) {
        char id[sizeof(talk_target.id)];
        xSemaphoreTake(state_lock, portMAX_DELAY);
        memcpy(id, talk_target.id, sizeof(id));
        bool selected = talk_target.selected, expired = talk_target.expired;
        xSemaphoreGive(state_lock);
        printf("next Talk agent: %s%s\n", selected ? id : "Gateway default",
            expired ? " (expired; select again)" : "");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "refresh") == 0 && argc <= 3) {
        size_t offset = 0;
        if (argc == 3) {
            if (argv[2][0] == '\0') return 1;
            for (const char *p = argv[2]; *p != '\0'; ++p) {
                if (*p < '0' || *p > '9' || offset > (SIZE_MAX - (size_t)(*p - '0')) / 10) return 1;
                offset = offset * 10 + (size_t)(*p - '0');
            }
        }
        if (room_agent_directory_refresh(offset) != ESP_OK) return 1;
    } else if (argc == 2 && strcmp(argv[1], "close") == 0) {
        room_agent_directory_cancel();
    } else if (argc != 1) {
        printf("usage: agents [refresh [offset] | close | select <id> | default | selection]\n");
        return 1;
    }
    room_agents_snapshot_t *snapshot = malloc(sizeof(*snapshot));
    if (snapshot == NULL) return 1;
    room_agent_directory_copy(snapshot);
    const char *states[] = {"idle", "offline", "loading", "ready", "empty", "error", "too-large", "timeout"};
    cJSON *root = cJSON_CreateObject();
    if (root != NULL) {
        cJSON_AddStringToObject(root, "state", states[snapshot->state]);
        cJSON_AddNumberToObject(root, "revision", snapshot->revision);
        cJSON_AddNumberToObject(root, "offset", snapshot->page.offset);
        cJSON_AddNumberToObject(root, "total", snapshot->page.total);
        cJSON *agents = cJSON_AddArrayToObject(root, "agents");
        for (size_t i = 0; agents != NULL && i < snapshot->page.count; ++i) {
            const esp_openclaw_agent_t *agent = &snapshot->page.agents[i];
            cJSON *item = cJSON_CreateObject();
            if (item == NULL) break;
            cJSON_AddStringToObject(item, "id", agent->id);
            cJSON_AddStringToObject(item, "name", agent->name);
            cJSON_AddStringToObject(item, "emoji", agent->emoji);
            cJSON_AddItemToArray(agents, item);
        }
    }
    free(snapshot);
    char *json = root != NULL ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (json == NULL) return 1;
    printf("agents=%s\n", json);
    free(json);
    return 0;
}

static bool conversation_index(const char *text, size_t *value)
{
    *value = 0;
    if (*text == '\0') return false;
    for (const char *p = text; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9' || *value > (SIZE_MAX - (size_t)(*p - '0')) / 10) return false;
        *value = *value * 10 + (size_t)(*p - '0');
    }
    return true;
}

static int conversations_console_command(int argc, char **argv)
{
    esp_err_t err = ESP_OK;
    if (argc == 3 && strcmp(argv[1], "open") == 0) {
        room_agents_snapshot_t *agents = malloc(sizeof(*agents));
        if (agents == NULL) return 1;
        room_agent_directory_copy(agents);
        err = room_conversations_open(argv[2], agents->revision);
        free(agents);
    } else if (argc == 3 && strcmp(argv[1], "refresh") == 0) {
        size_t offset;
        err = conversation_index(argv[2], &offset) ? room_conversations_refresh(offset) : ESP_ERR_INVALID_ARG;
    } else if (argc == 3 && strcmp(argv[1], "preview") == 0) {
        size_t index;
        if (!conversation_index(argv[2], &index)) return 1;
        room_conversations_snapshot_t *view = malloc(sizeof(*view));
        if (view == NULL) return 1;
        room_conversations_copy(view);
        err = room_conversations_preview(index, view->revision);
        free(view);
    } else if (argc == 2 && strcmp(argv[1], "close") == 0) {
        room_conversations_close();
    } else if (argc != 1) {
        printf("usage: conversations [open <agentId> | refresh <offset> | preview <index> | close]\n");
        return 1;
    }
    if (err != ESP_OK) {
        printf("conversations: %s\n", esp_err_to_name(err));
        return 1;
    }
    room_conversations_snapshot_t *view = malloc(sizeof(*view));
    if (view == NULL) return 1;
    room_conversations_copy(view);
    const char *states[] = {"idle", "offline", "loading", "ready", "empty", "error", "too-large", "timeout"};
    const char *statuses[] = {"ok", "empty", "missing", "error"};
    cJSON *root = cJSON_CreateObject();
    if (root != NULL) {
        cJSON_AddStringToObject(root, "state", states[view->state]);
        cJSON_AddStringToObject(root, "previewState", states[view->preview_state]);
        cJSON_AddStringToObject(root, "agentId", view->agent.id);
        cJSON_AddNumberToObject(root, "revision", view->revision);
        cJSON_AddNumberToObject(root, "offset", view->page.offset);
        cJSON_AddBoolToObject(root, "hasMore", view->page.has_more);
        cJSON_AddNumberToObject(root, "nextOffset", view->page.next_offset);
        cJSON *sessions = cJSON_AddArrayToObject(root, "sessions");
        for (size_t i = 0; sessions != NULL && i < view->page.count; ++i) {
            cJSON *item = cJSON_CreateObject();
            if (item == NULL) break;
            cJSON_AddStringToObject(item, "key", view->page.sessions[i].key);
            cJSON_AddStringToObject(item, "title", view->page.sessions[i].title);
            cJSON_AddItemToArray(sessions, item);
        }
        cJSON_AddStringToObject(root, "previewStatus", statuses[view->preview.status]);
        cJSON *items = cJSON_AddArrayToObject(root, "items");
        for (size_t i = 0; items != NULL && i < view->preview.count; ++i) {
            cJSON *item = cJSON_CreateObject();
            if (item == NULL) break;
            cJSON_AddNumberToObject(item, "role", view->preview.items[i].role);
            cJSON_AddStringToObject(item, "text", view->preview.items[i].text);
            cJSON_AddItemToArray(items, item);
        }
    }
    free(view);
    char *json = root != NULL ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (json == NULL) return 1;
    printf("conversations=%s\n", json);
    free(json);
    return 0;
}

static esp_err_t register_room_console_commands(void)
{
    static const esp_console_cmd_t COMMANDS[] = {
        {
            .command = "conversations",
            .help = "Read an agent's conversations without changing Talk",
            .hint = "open <agentId> | refresh <offset> | preview <index> | close",
            .func = conversations_console_command,
        },
        {
            .command = "brightness",
            .help = "Set Watch awake brightness until restart; idle sleep and camera minimum stay enabled",
            .hint = "<10..100>",
            .func = brightness_console_command,
        },
        {
            .command = "menu",
            .help = "Open a local watch menu page without starting Talk",
            .hint = "[menu|agents|conversations|preview|devices|device|settings|display|brightness|reading|density|messaging|connection|canvas|home]",
            .func = menu_console_command,
        },
        {
            .command = "devices",
            .help = "Inspect authorized Gateway devices without invoking commands",
            .hint = "refresh | close",
            .func = devices_console_command,
        },
        {
            .command = "agents",
            .help = "Inspect agents and choose the next Talk target",
            .hint = "refresh [offset] | close | select <id> | default | selection",
            .func = agents_console_command,
        },
        {
            .command = "messaging",
            .help = "Request Gateway-approved messaging access or inspect its status",
            .hint = "[request]",
            .func = messaging_console_command,
        },
        {
            .command = "voice-actions",
            .help = "Prepare Gateway-approved voice write access before enabling the build setting",
            .hint = "request | complete <requestId>",
            .func = voice_actions_console_command,
        },
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
    snapshot->wifi_ap_info_valid = wifi.ap_info_valid;
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
    if (nvs_err != ESP_OK) {
        esp_rom_printf(DRAM_STR("nvs_init_diag err=%d\n"), (int)nvs_err);
    }
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
    ESP_ERROR_CHECK(create_connection_timers());
    /* Media setup can use PSRAM; teardown and NVS-backed operator startup
     * share one internal stack. */
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
        6144,
        NULL,
        6,
        NULL);
    ESP_ERROR_CHECK(teardown_task == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    room_ui_set_talk_action_handler(request_local_talk);
    room_ui_set_voice_action_handler(request_local_voice);
    room_ui_set_messaging_handler(request_local_messaging);
    room_ui_set_agents_action_handler(request_local_agents);
    room_ui_set_devices_action_handler(request_local_devices);
    room_ui_set_conversation_handler(request_local_conversation);
    room_ui_set_agent_selection_handler(request_local_agent_selection);
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
            xSemaphoreTake(state_lock, portMAX_DELAY);
            home_wifi_state = ROOM_UI_WIFI_UNAVAILABLE;
            xSemaphoreGive(state_lock);
            refresh_home_facts();
            room_ui_set(ROOM_UI_ERROR, "Wi-Fi coprocessor unavailable");
            return network_err;
        }
    }
    ESP_ERROR_CHECK(esp_openclaw_node_wifi_start());
    /* Register after the station helper so its status reflects each event first. */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, home_network_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, home_network_event, NULL));
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
    refresh_wifi_facts(NULL, 0);

    ESP_LOGI(TAG, "initializing room media");
    esp_err_t media_err = room_media_init(on_wake, NULL);
    if (media_err != ESP_OK) {
        /* Canvas must stay usable when the unvalidated audio path fails; wake
         * and Talk stay off until the microphone path works. */
        ESP_LOGE(
            TAG,
            "room media init failed: %s; Talk wake is disabled",
            esp_err_to_name(media_err));
    }
    xSemaphoreTake(state_lock, portMAX_DELAY);
    media_ready = media_err == ESP_OK;
    media_initialized = true;
    xSemaphoreGive(state_lock);
    refresh_home_facts();

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
