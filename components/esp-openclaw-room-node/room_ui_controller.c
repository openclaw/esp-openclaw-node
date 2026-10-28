#include "room_ui_controller.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "room_canvas.h"
#include "room_diagnostics.h"
#include "room_face.h"
#include "room_board.h"
#include "room_watch_home.h"
#include "room_watch_status.h"
#include "room_watch_navigation.h"
#include "room_watch_settings.h"
#include "openclaw_reading.h"
#include "openclaw_density.h"
#include "room_watch_agents.h"
#include "room_watch_devices.h"
#include "room_watch_conversations.h"

static bool watch_navigation_active(void);
static void start_text_hint(uint32_t show_ms);
static void room_ui_paint_and_unlock(void);
static int room_ui_target_brightness(room_ui_state_t state, bool canvas_active);

/* Tap toggles retained Canvas content; hold opens local diagnostics. */
static void room_ui_status_clicked(lv_event_t *event)
{
    if (watch_navigation_active()) return;
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_LONG_PRESSED) {
        lv_indev_t *indev = lv_indev_active();
        if (indev != NULL) lv_indev_wait_release(indev);
        room_diagnostics_open();
    } else if (code == LV_EVENT_CLICKED && !room_diagnostics_is_open()) {
        room_canvas_view_toggle();
    }
}

static const char *TAG = "room_ui";
#define ROOM_UI_CAMERA_MIN_BRIGHTNESS 40
static lv_obj_t *status_label;
static lv_obj_t *gateway_label;
static lv_obj_t *home;
static lv_obj_t *home_wifi;
static lv_obj_t *home_gateway;
static lv_obj_t *home_talk;
static lv_obj_t *home_detail;
static lv_obj_t *talk_pill;
static lv_obj_t *talk_pill_label;
static lv_obj_t *talk_target_label;
static lv_obj_t *camera_indicator;
static bool animated_face_enabled;
static bool watch_home_enabled;
static uint8_t idle_brightness;
static int watch_brightness = -1;
static bool watch_brightness_failed;
/* Guarded by state_mux like the state/detail facts. */
static char gateway_text[64];
/* Last state/detail survive canvas mode so leaving it restores the live Talk
 * state instead of forcing idle while a call is still active. Guarded by
 * their own spinlock so a display-lock timeout can never DROP a transition:
 * losing the operator-ready IDLE update once left the face stuck in the
 * connecting look forever. Painting may fail and retry; the fact may not.  */
static portMUX_TYPE state_mux = portMUX_INITIALIZER_UNLOCKED;
static room_ui_state_t current_state = ROOM_UI_IDLE;
static char current_detail[48];
static bool diagnostics_open;
static bool text_hint_active;
static bool camera_indicator_active;
static room_ui_facts_t current_facts;
static esp_timer_handle_t text_hint_timer;
static void (*talk_action_handler)(bool start, uint64_t recipient_revision);
typedef enum { TALK_CONTROL_NONE, TALK_CONTROL_START, TALK_CONTROL_STOP, TALK_CONTROL_RETURN } talk_control_action_t;
/* Painted intent is display-lock owned; a press is invalidated with the facts. */
static talk_control_action_t painted_talk_action;
static talk_control_action_t pressed_talk_action;
static room_watch_page_t watch_page = ROOM_WATCH_HOME;
static room_watch_page_t watch_display_parent = ROOM_WATCH_SETTINGS;
static room_watch_page_t watch_connection_parent = ROOM_WATCH_MENU;
static room_watch_page_t watch_messaging_parent = ROOM_WATCH_SETTINGS;
static room_messaging_snapshot_t current_messaging, painted_messaging;
static void (*messaging_handler)(unsigned revision);
static bool messaging_held, messaging_pending;

/* Called with state_mux held; subpages return to their actual entry point. */
static room_watch_page_t watch_parent(room_watch_page_t page)
{
    if (page == ROOM_WATCH_DISPLAY) return watch_display_parent;
    if (page == ROOM_WATCH_BRIGHTNESS || page == ROOM_WATCH_READING || page == ROOM_WATCH_DENSITY) return ROOM_WATCH_DISPLAY;
    if (page == ROOM_WATCH_MESSAGING) return watch_messaging_parent;
    if (page == ROOM_WATCH_CONNECTION) return watch_connection_parent;
    if (page == ROOM_WATCH_LOCAL_DEVICE) return ROOM_WATCH_DEVICES;
    if (page == ROOM_WATCH_DEVICES || page == ROOM_WATCH_SETTINGS || page == ROOM_WATCH_CALL) return ROOM_WATCH_MENU;
    return ROOM_WATCH_HOME;
}
static int pressed_watch_page = -1;
static void (*agents_action_handler)(bool open, size_t offset);
static room_agents_snapshot_t current_agents, painted_agents;
static room_ui_talk_target_t current_target;
/* A changed recipient must reach the display before accepting a new Start. */
static uint64_t painted_target_revision;
static room_voice_snapshot_t current_voice, painted_voice;
static void (*voice_action_handler)(room_voice_action_t, uint32_t, uint64_t, uint64_t);
/* Gesture state is display-owned. Audio ownership stays with the supervisor. */
static bool voice_held, voice_started;
static uint32_t voice_generation, voice_canceled_generation, voice_pressed_at;
static uint64_t voice_revision, voice_owner;
static int32_t voice_start_y;
static uint32_t discard_pressed_generation;
static uint32_t send_pressed_generation;
static room_voice_state_t send_pressed_state;
static char voice_conversation_agent[65];
static bool selection_pending, selection_failed;
static void (*agent_selection_handler)(const char *id, unsigned revision);
static int pressed_agent_action = -100;
static unsigned pressed_agent_revision;
static room_agents_state_t pressed_agent_state;
static void (*devices_action_handler)(bool open, unsigned generation);
static esp_openclaw_devices_snapshot_t *current_devices, *painted_devices;
static unsigned device_generation;
static int pressed_device_action = -100;
static unsigned pressed_device_revision;
static esp_openclaw_devices_state_t pressed_device_state;

void room_ui_set_messaging_handler(void (*handler)(unsigned revision))
{
    messaging_handler = handler;
}

void room_ui_store_messaging(const room_messaging_snapshot_t *snapshot)
{
    taskENTER_CRITICAL(&state_mux);
    bool changed = messaging_pending || current_messaging.revision != snapshot->revision ||
        current_messaging.state != snapshot->state || current_messaging.connected != snapshot->connected ||
        current_messaging.can_request != snapshot->can_request || current_messaging.busy != snapshot->busy;
    current_messaging = *snapshot;
    if (changed) messaging_held = false;
    messaging_pending = false;
    taskEXIT_CRITICAL(&state_mux);
    if (changed) room_ui_refresh();
}

void room_ui_set_devices_action_handler(void (*handler)(bool open, unsigned generation))
{
    devices_action_handler = handler;
}

bool room_ui_wants_devices(void)
{
    taskENTER_CRITICAL(&state_mux);
    bool wanted = current_devices != NULL;
    taskEXIT_CRITICAL(&state_mux);
    return wanted;
}

void room_ui_store_devices(const esp_openclaw_devices_snapshot_t *snapshot, unsigned generation)
{
    taskENTER_CRITICAL(&state_mux);
    bool changed = current_devices != NULL && generation == device_generation &&
        (current_devices->revision != snapshot->revision || current_devices->state != snapshot->state);
    if (changed) {
        *current_devices = *snapshot;
        pressed_device_action = -100;
    }
    taskEXIT_CRITICAL(&state_mux);
    if (changed) room_ui_refresh();
}

void room_ui_devices_failed(unsigned generation)
{
    taskENTER_CRITICAL(&state_mux);
    if (current_devices != NULL && generation == device_generation) {
        current_devices->state = ESP_OPENCLAW_DEVICES_ERROR;
        pressed_device_action = -100;
    }
    taskEXIT_CRITICAL(&state_mux);
    room_ui_refresh();
}

/* Only the display owner allocates and releases these view buffers. */
static void close_devices(void)
{
    taskENTER_CRITICAL(&state_mux);
    esp_openclaw_devices_snapshot_t *current = current_devices, *painted = painted_devices;
    current_devices = painted_devices = NULL;
    pressed_device_action = -100;
    unsigned generation = ++device_generation;
    taskEXIT_CRITICAL(&state_mux);
    room_watch_devices_render(NULL, true, 432);
    free(current);
    free(painted);
    if (devices_action_handler != NULL) devices_action_handler(false, generation);
}

static void open_devices(void)
{
    close_devices();
    esp_openclaw_devices_snapshot_t *current = calloc(1, sizeof(*current));
    esp_openclaw_devices_snapshot_t *painted = calloc(1, sizeof(*painted));
    if (current == NULL || painted == NULL) { free(current); free(painted); current = painted = NULL; }
    if (current != NULL) { current->revision = UINT32_MAX; current->state = ESP_OPENCLAW_DEVICES_LOADING; }
    taskENTER_CRITICAL(&state_mux);
    current_devices = current;
    painted_devices = painted;
    unsigned generation = ++device_generation;
    taskEXIT_CRITICAL(&state_mux);
    room_watch_devices_reset();
    if (current != NULL && devices_action_handler != NULL) devices_action_handler(true, generation);
}

static bool conversations_open;
static bool conversation_resume_failed;
static room_conversations_snapshot_t *current_conversations, *painted_conversations;
static int pressed_chat_action = -100;
static unsigned conversation_generation;
static room_conversation_context_t current_reader_context;
static unsigned painted_reader_epoch, pressed_reader_epoch, resume_reader_epoch;
static room_home_action_t painted_home_action, pressed_home_action;
static void (*conversation_handler)(room_ui_conversation_action_t, const char *, unsigned, size_t, unsigned);

void room_ui_store_conversation_context(const room_conversation_context_t *context)
{
    taskENTER_CRITICAL(&state_mux);
    bool changed = context->epoch != current_reader_context.epoch ||
        strcmp(context->agent_name, current_reader_context.agent_name) != 0;
    if (changed) {
        current_reader_context = *context;
        pressed_reader_epoch = 0;
        pressed_home_action = ROOM_HOME_NONE;
    }
    taskEXIT_CRITICAL(&state_mux);
    if (changed) room_ui_refresh();
}

void room_ui_set_conversation_handler(void (*handler)(room_ui_conversation_action_t,
    const char *, unsigned, size_t, unsigned))
{
    conversation_handler = handler;
}

bool room_ui_wants_conversations(void)
{
    taskENTER_CRITICAL(&state_mux);
    bool wanted = current_conversations != NULL;
    taskEXIT_CRITICAL(&state_mux);
    return wanted;
}

void room_ui_store_conversations(const room_conversations_snapshot_t *snapshot, unsigned generation)
{
    taskENTER_CRITICAL(&state_mux);
    room_conversations_snapshot_t *current = current_conversations;
    /* A new view must not consume a snapshot copied before its open intent. */
    bool changed = current != NULL && generation == conversation_generation && (current->revision != snapshot->revision ||
        current->preview_revision != snapshot->preview_revision || current->selected != snapshot->selected ||
        current->state != snapshot->state ||
        current->preview_state != snapshot->preview_state ||
        memcmp(&current->creation, &snapshot->creation, sizeof(current->creation)) != 0);
    if (changed) {
        *current = *snapshot;
        pressed_chat_action = -100;
    }
    taskEXIT_CRITICAL(&state_mux);
    if (changed) room_ui_refresh();
}

void room_ui_conversations_failed(unsigned generation)
{
    taskENTER_CRITICAL(&state_mux);
    if (current_conversations != NULL && generation == conversation_generation) current_conversations->state = current_conversations->preview_state = ROOM_AGENTS_ERROR;
    if (watch_page == ROOM_WATCH_CONVERSATIONS && generation == conversation_generation) conversation_resume_failed = true;
    pressed_chat_action = -100;
    taskEXIT_CRITICAL(&state_mux);
    room_ui_refresh();
}

/* Display callbacks own allocation and release; the supervisor only copies
 * into the current buffer while holding state_mux. */
static void close_conversations(bool cancel)
{
    taskENTER_CRITICAL(&state_mux);
    room_conversations_snapshot_t *current = current_conversations, *painted = painted_conversations;
    current_conversations = painted_conversations = NULL;
    conversations_open = false;
    pressed_chat_action = -100;
    taskEXIT_CRITICAL(&state_mux);
    room_watch_conversations_render(NULL, true, 432);
    free(current);
    free(painted);
    if (cancel && conversation_handler != NULL) conversation_handler(ROOM_CONVERSATIONS_CLOSE, "", 0, 0, conversation_generation);
}

static void open_conversations(const char *id, unsigned revision)
{
    room_conversations_snapshot_t *current = calloc(1, sizeof(*current));
    room_conversations_snapshot_t *painted = calloc(1, sizeof(*painted));
    if (current == NULL || painted == NULL) { free(current); free(painted); current = painted = NULL; }
    if (current != NULL) { current->revision = UINT32_MAX; current->state = ROOM_AGENTS_LOADING; }
    close_conversations(false);
    taskENTER_CRITICAL(&state_mux);
    current_conversations = current;
    painted_conversations = painted;
    conversations_open = true;
    ++conversation_generation;
    pressed_agent_action = -100;
    taskEXIT_CRITICAL(&state_mux);
    room_watch_conversations_reset(false);
    if (current != NULL && conversation_handler != NULL)
        conversation_handler(ROOM_CONVERSATIONS_OPEN, id, revision, 0, conversation_generation);
}

void room_ui_set_agents_action_handler(void (*handler)(bool open, size_t offset))
{
    agents_action_handler = handler;
}

void room_ui_set_agent_selection_handler(void (*handler)(const char *id, unsigned revision))
{
    agent_selection_handler = handler;
}

void room_ui_store_talk_target(const room_ui_talk_target_t *target)
{
    taskENTER_CRITICAL(&state_mux);
    if (target->revision != current_target.revision ||
        target->selected != current_target.selected || target->expired != current_target.expired ||
        strcmp(target->id, current_target.id) != 0 || strcmp(target->name, current_target.name) != 0 ||
        target->avatar.kind != current_target.avatar.kind ||
        strcmp(target->avatar.text, current_target.avatar.text) != 0) {
        current_target = *target;
        pressed_talk_action = TALK_CONTROL_NONE;
    }
    taskEXIT_CRITICAL(&state_mux);
}

void room_ui_agent_selection_complete(esp_err_t result)
{
    taskENTER_CRITICAL(&state_mux);
    selection_pending = false;
    selection_failed = result != ESP_OK;
    pressed_talk_action = TALK_CONTROL_NONE;
    taskEXIT_CRITICAL(&state_mux);
}

void room_ui_store_agents(const room_agents_snapshot_t *snapshot)
{
    taskENTER_CRITICAL(&state_mux);
    bool changed = current_agents.revision != snapshot->revision || current_agents.state != snapshot->state;
    if (changed) {
        current_agents = *snapshot;
        pressed_agent_action = -100;
    }
    taskEXIT_CRITICAL(&state_mux);
    if (changed) room_ui_refresh();
}

static void request_agents_page(size_t offset)
{
    taskENTER_CRITICAL(&state_mux);
    current_agents.state = ROOM_AGENTS_LOADING;
    memset(&current_agents.page, 0, sizeof(current_agents.page));
    current_agents.page.offset = offset;
    pressed_agent_action = -100;
    taskEXIT_CRITICAL(&state_mux);
    if (agents_action_handler != NULL) agents_action_handler(true, offset);
}


static bool watch_navigation_active(void)
{
    taskENTER_CRITICAL(&state_mux);
    bool active = watch_home_enabled && watch_page != ROOM_WATCH_HOME;
    taskEXIT_CRITICAL(&state_mux);
    return active;
}

static esp_err_t watch_navigation_show_context(room_watch_page_t page, unsigned reader_epoch)
{
    if (!room_board_display_lock(100)) return ESP_ERR_TIMEOUT;
    if (page == ROOM_WATCH_DEVICE_STATUS) {
        esp_err_t err = room_diagnostics_open();
        room_board_display_unlock();
        return err;
    }
    room_canvas_diagnostics_snapshot_t canvas;
    room_canvas_get_diagnostics(&canvas);
    bool show_canvas = false;
    if (page == ROOM_WATCH_CANVAS) show_canvas = canvas.retained_kind != ROOM_CANVAS_RETAINED_NONE;
    if (canvas.active && !show_canvas) {
        char *payload = NULL;
        esp_openclaw_node_error_t error = {0};
        esp_err_t err = room_canvas_hide(&payload, &error);
        free(payload);
        if (err != ESP_OK) {
            room_board_display_unlock();
            return err;
        }
    } else if (!canvas.active && show_canvas) {
        room_canvas_view_toggle();
        if (!room_canvas_is_active()) {
            room_board_display_unlock();
            return ESP_ERR_INVALID_STATE;
        }
    }
    taskENTER_CRITICAL(&state_mux);
    if ((page == ROOM_WATCH_DISPLAY || page == ROOM_WATCH_BRIGHTNESS || page == ROOM_WATCH_READING || page == ROOM_WATCH_DENSITY) &&
        watch_page != ROOM_WATCH_DISPLAY && watch_page != ROOM_WATCH_BRIGHTNESS && watch_page != ROOM_WATCH_READING && watch_page != ROOM_WATCH_DENSITY)
        watch_display_parent = watch_page == ROOM_WATCH_LOCAL_DEVICE ? ROOM_WATCH_LOCAL_DEVICE : ROOM_WATCH_SETTINGS;
    if (page == ROOM_WATCH_MESSAGING && watch_page != ROOM_WATCH_MESSAGING)
        watch_messaging_parent = watch_page == ROOM_WATCH_LOCAL_DEVICE ? ROOM_WATCH_LOCAL_DEVICE : ROOM_WATCH_SETTINGS;
    messaging_held = false;
    if (page == ROOM_WATCH_CONNECTION && watch_page != ROOM_WATCH_CONNECTION)
        watch_connection_parent = watch_page == ROOM_WATCH_LOCAL_DEVICE ? ROOM_WATCH_LOCAL_DEVICE :
            watch_page == ROOM_WATCH_SETTINGS ? ROOM_WATCH_SETTINGS : ROOM_WATCH_MENU;
    bool leaving_agents = watch_page == ROOM_WATCH_AGENTS && page != ROOM_WATCH_AGENTS;
    bool entering_agents = watch_page != ROOM_WATCH_AGENTS && page == ROOM_WATCH_AGENTS;
    bool leaving_devices = watch_page == ROOM_WATCH_DEVICES && page != ROOM_WATCH_DEVICES;
    bool entering_devices = watch_page != ROOM_WATCH_DEVICES && page == ROOM_WATCH_DEVICES;
    bool suspend_reader = leaving_agents && conversations_open;
    bool resume_reader = page == ROOM_WATCH_CONVERSATIONS;
    resume_reader_epoch = resume_reader ? reader_epoch : 0;
    pressed_reader_epoch = 0;
    pressed_home_action = ROOM_HOME_NONE;
    if (resume_reader) conversation_resume_failed = false;
    if (watch_page == ROOM_WATCH_CONVERSATIONS || suspend_reader || resume_reader) ++conversation_generation;
    bool wake_home = page == ROOM_WATCH_HOME;
    if (page != ROOM_WATCH_AGENTS) voice_conversation_agent[0] = '\0';
    watch_page = show_canvas ? ROOM_WATCH_HOME : page;
    pressed_watch_page = -1;
    pressed_talk_action = TALK_CONTROL_NONE;
    taskEXIT_CRITICAL(&state_mux);
    if (leaving_devices) close_devices();
    if (entering_devices) open_devices();
    if (entering_agents) { room_watch_agents_reset(); request_agents_page(0); }
    if (suspend_reader || (leaving_agents && resume_reader)) {
        if (suspend_reader) room_watch_conversations_suspend();
        close_conversations(false);
        if (conversation_handler != NULL)
            conversation_handler(ROOM_CONVERSATIONS_SUSPEND, "", 0, 0, conversation_generation);
    } else if (leaving_agents && agents_action_handler != NULL) agents_action_handler(false, 0);
    if (resume_reader && conversation_handler != NULL)
        conversation_handler(ROOM_CONVERSATIONS_RESUME, "", 0, 0, conversation_generation);
    if (wake_home) start_text_hint(10000);
    room_ui_paint_and_unlock();
    return ESP_OK;
}

static esp_err_t watch_navigation_show(room_watch_page_t page)
{
    return watch_navigation_show_context(page, 0);
}

esp_err_t room_ui_watch_menu(const char *page)
{
    room_watch_page_t requested;
    if (page == NULL) return ESP_ERR_INVALID_ARG;
    if (strcmp(page, "menu") == 0) requested = ROOM_WATCH_MENU;
    else if (strcmp(page, "agents") == 0) requested = ROOM_WATCH_AGENTS;
    else if (strcmp(page, "conversations") == 0) requested = ROOM_WATCH_CONVERSATIONS;
    else if (strcmp(page, "devices") == 0) requested = ROOM_WATCH_DEVICES;
    else if (strcmp(page, "device") == 0) requested = ROOM_WATCH_LOCAL_DEVICE;
    else if (strcmp(page, "settings") == 0) requested = ROOM_WATCH_SETTINGS;
    else if (strcmp(page, "display") == 0) requested = ROOM_WATCH_DISPLAY;
    else if (strcmp(page, "brightness") == 0) requested = ROOM_WATCH_BRIGHTNESS;
    else if (strcmp(page, "reading") == 0) requested = ROOM_WATCH_READING;
    else if (strcmp(page, "density") == 0) requested = ROOM_WATCH_DENSITY;
    else if (strcmp(page, "messaging") == 0) requested = ROOM_WATCH_MESSAGING;
    else if (strcmp(page, "connection") == 0) requested = ROOM_WATCH_CONNECTION;
    else if (strcmp(page, "call") == 0) requested = ROOM_WATCH_CALL;
    else if (strcmp(page, "canvas") == 0) requested = ROOM_WATCH_CANVAS;
    else if (strcmp(page, "home") == 0) requested = ROOM_WATCH_HOME;
    else return ESP_ERR_INVALID_ARG;
    taskENTER_CRITICAL(&state_mux);
    bool available = watch_home_enabled && !diagnostics_open;
    taskEXIT_CRITICAL(&state_mux);
    if (!available) return ESP_ERR_INVALID_STATE;
    return watch_navigation_show(requested);
}

esp_err_t room_ui_watch_reader(const room_conversations_snapshot_t *snapshot, bool preview, unsigned generation)
{
    if (snapshot == NULL || snapshot->agent.id[0] == '\0' ||
        (preview && (snapshot->state != ROOM_AGENTS_READY || snapshot->selected >= snapshot->page.count ||
                     snapshot->preview_state == ROOM_AGENTS_IDLE))) return ESP_ERR_INVALID_STATE;
    if (!room_board_display_lock(150)) return ESP_ERR_TIMEOUT;
    taskENTER_CRITICAL(&state_mux);
    bool valid = watch_home_enabled && !diagnostics_open && watch_page == ROOM_WATCH_AGENTS &&
        current_agents.state == ROOM_AGENTS_READY && painted_agents.state == ROOM_AGENTS_READY &&
        current_agents.revision == painted_agents.revision;
    bool member = false;
    for (size_t i = 0; valid && i < current_agents.page.count; ++i)
        if (strcmp(current_agents.page.agents[i].id, snapshot->agent.id) == 0) member = true;
    taskEXIT_CRITICAL(&state_mux);
    if (!member) { room_board_display_unlock(); return ESP_ERR_INVALID_STATE; }
    room_conversations_snapshot_t *current = malloc(sizeof(*current));
    room_conversations_snapshot_t *painted = malloc(sizeof(*painted));
    if (current == NULL || painted == NULL) {
        free(current); free(painted); room_board_display_unlock(); return ESP_ERR_NO_MEM;
    }
    *current = *painted = *snapshot;
    close_conversations(false);
    taskENTER_CRITICAL(&state_mux);
    current_conversations = current;
    painted_conversations = painted;
    conversations_open = true;
    conversation_generation = generation;
    pressed_agent_action = -100;
    taskEXIT_CRITICAL(&state_mux);
    room_watch_agents_focus(snapshot->agent.id);
    room_watch_conversations_reset(preview);
    room_ui_paint_and_unlock();
    return ESP_OK;
}

bool room_ui_wants_conversation_resume(unsigned generation)
{
    taskENTER_CRITICAL(&state_mux);
    bool wanted = watch_home_enabled && watch_page == ROOM_WATCH_CONVERSATIONS &&
        !diagnostics_open && conversation_generation == generation;
    taskEXIT_CRITICAL(&state_mux);
    return wanted;
}

esp_err_t room_ui_watch_resume(const room_agents_snapshot_t *agents,
    const room_conversations_snapshot_t *reader, unsigned epoch, unsigned generation)
{
    if (!room_board_display_lock(150)) return ESP_ERR_TIMEOUT;
    if (!room_ui_wants_conversation_resume(generation)) {
        room_board_display_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    taskENTER_CRITICAL(&state_mux);
    unsigned expected_epoch = resume_reader_epoch;
    taskEXIT_CRITICAL(&state_mux);
    if (expected_epoch != 0 && (reader == NULL || epoch != expected_epoch)) {
        esp_err_t err = watch_navigation_show(ROOM_WATCH_HOME);
        room_board_display_unlock();
        return err;
    }
    if (agents == NULL || reader == NULL) {
        /* Navigation takes and releases another recursive display lock. */
        esp_err_t err = watch_navigation_show(ROOM_WATCH_AGENTS);
        room_board_display_unlock();
        return err;
    }
    room_conversations_snapshot_t *current = malloc(sizeof(*current));
    room_conversations_snapshot_t *painted = malloc(sizeof(*painted));
    if (current == NULL || painted == NULL) {
        free(current); free(painted);
        taskENTER_CRITICAL(&state_mux);
        conversation_resume_failed = true;
        taskEXIT_CRITICAL(&state_mux);
        room_ui_paint_and_unlock();
        return ESP_ERR_NO_MEM;
    }
    if (!room_ui_wants_conversation_resume(generation) || !room_conversations_resume_current(epoch)) {
        free(current); free(painted);
        room_board_display_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    *current = *painted = *reader;
    close_conversations(false);
    taskENTER_CRITICAL(&state_mux);
    current_agents = *agents;
    current_conversations = current;
    painted_conversations = painted;
    conversations_open = true;
    watch_page = ROOM_WATCH_AGENTS;
    pressed_watch_page = -1;
    pressed_agent_action = -100;
    pressed_talk_action = TALK_CONTROL_NONE;
    taskEXIT_CRITICAL(&state_mux);
    room_watch_agents_focus(reader->agent.id);
    room_watch_conversations_resume(reader);
    room_ui_paint_and_unlock();
    return ESP_OK;
}

static void watch_navigation_input(lv_event_t *event, room_watch_page_t page)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESS_LOST && code != LV_EVENT_SHORT_CLICKED) return;
    bool wake = code == LV_EVENT_PRESSED && room_board_display_is_dark();
    bool activate = false;
    taskENTER_CRITICAL(&state_mux);
    bool brightness_action = page == ROOM_WATCH_DIMMER || page == ROOM_WATCH_BRIGHTER;
    bool reading_action = page == ROOM_WATCH_TEXT_STANDARD || page == ROOM_WATCH_TEXT_LARGE;
    bool density_action = page == ROOM_WATCH_COMPACT || page == ROOM_WATCH_COMFORTABLE;
    if ((brightness_action && watch_page != ROOM_WATCH_BRIGHTNESS) ||
        (reading_action && watch_page != ROOM_WATCH_READING) ||
        (density_action && watch_page != ROOM_WATCH_DENSITY)) {
        taskEXIT_CRITICAL(&state_mux);
        return;
    }
    if (code == LV_EVENT_PRESSED) pressed_watch_page = wake || diagnostics_open ? -1 : (int)page;
    else {
        activate = code == LV_EVENT_SHORT_CLICKED && !diagnostics_open && pressed_watch_page == (int)page;
        pressed_watch_page = -1;
    }
    taskEXIT_CRITICAL(&state_mux);
    if (wake) { room_ui_show_face_hint(10000); return; }
    if (!activate) return;
    if (density_action) {
        openclaw_density_set(page == ROOM_WATCH_COMFORTABLE ? OPENCLAW_DENSITY_COMFORTABLE : OPENCLAW_DENSITY_COMPACT);
        room_ui_refresh();
    } else if (reading_action) {
        openclaw_reading_set_size(page == ROOM_WATCH_TEXT_LARGE ? OPENCLAW_READING_LARGE : OPENCLAW_READING_STANDARD);
        room_ui_refresh();
    } else if (brightness_action) {
        taskENTER_CRITICAL(&state_mux);
        int percent = watch_brightness < 0 ? ROOM_CANVAS_ACTIVE_BRIGHTNESS : watch_brightness;
        taskEXIT_CRITICAL(&state_mux);
        percent += page == ROOM_WATCH_BRIGHTER ? 5 : -5;
        if (percent < 10) percent = 10;
        if (percent > 100) percent = 100;
        (void)room_ui_watch_brightness(percent);
    } else (void)watch_navigation_show(page);
}

static void watch_messaging_clicked(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESS_LOST && code != LV_EVENT_SHORT_CLICKED) return;
    bool wake = code == LV_EVENT_PRESSED && room_board_display_is_dark();
    bool activate = false;
    unsigned revision;
    taskENTER_CRITICAL(&state_mux);
    bool current = watch_page == ROOM_WATCH_MESSAGING && !diagnostics_open && !messaging_pending &&
        messaging_handler != NULL && painted_messaging.can_request && current_messaging.can_request &&
        painted_messaging.connected == current_messaging.connected &&
        painted_messaging.state == current_messaging.state && painted_messaging.revision == current_messaging.revision;
    revision = painted_messaging.revision;
    if (code == LV_EVENT_PRESSED) messaging_held = !wake && current;
    else {
        activate = code == LV_EVENT_SHORT_CLICKED && messaging_held && current;
        messaging_held = false;
        if (activate) messaging_pending = true;
    }
    taskEXIT_CRITICAL(&state_mux);
    if (wake) { room_ui_show_face_hint(10000); return; }
    if (activate) {
        messaging_handler(revision);
        room_ui_refresh();
    }
}

static void watch_home_menu_clicked(lv_event_t *event)
{
    watch_navigation_input(event, ROOM_WATCH_MENU);
}

static void watch_home_agent_clicked(lv_event_t *event)
{
    if (!lv_obj_is_visible(lv_event_get_current_target(event))) return;
    watch_navigation_input(event, ROOM_WATCH_AGENTS);
}

/* Reused by paint and input so an old fallback cannot outlive voice/call state. */
static room_home_action_t watch_home_action_locked(bool reader_current)
{
    bool call = current_facts.talk == ROOM_UI_TALK_CONNECTING || current_facts.talk == ROOM_UI_TALK_ACTIVE ||
        current_facts.talk == ROOM_UI_TALK_STOPPING;
    bool voice_ready = voice_action_handler != NULL && current_facts.talk == ROOM_UI_TALK_READY &&
        current_facts.wifi == ROOM_UI_WIFI_CONNECTED && current_facts.gateway == ROOM_UI_GATEWAY_CONNECTED &&
        !selection_pending && !current_target.expired && !messaging_pending && !current_messaging.busy &&
        current_voice.state != ROOM_VOICE_UNAVAILABLE && current_state != ROOM_UI_ERROR && current_state != ROOM_UI_SETUP;
    if (call || voice_ready || (voice_action_handler != NULL && (voice_held || current_voice.state >= ROOM_VOICE_STARTING)))
        return ROOM_HOME_NONE;
    if (current_state == ROOM_UI_ERROR || current_state == ROOM_UI_SETUP ||
        current_facts.wifi != ROOM_UI_WIFI_CONNECTED || current_facts.gateway != ROOM_UI_GATEWAY_CONNECTED)
        return ROOM_HOME_CONNECTION;
    return reader_current ? ROOM_HOME_CONTINUE : ROOM_HOME_AGENTS;
}

static void watch_home_continue_clicked(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESS_LOST && code != LV_EVENT_SHORT_CLICKED) return;
    bool wake = code == LV_EVENT_PRESSED && room_board_display_is_dark();
    taskENTER_CRITICAL(&state_mux);
    unsigned epoch = current_reader_context.epoch;
    taskEXIT_CRITICAL(&state_mux);
    bool reader_current = epoch != 0 && room_conversations_resume_current(epoch);
    bool object_visible = lv_obj_is_visible(lv_event_get_current_target(event));
    room_home_action_t requested = ROOM_HOME_NONE;
    taskENTER_CRITICAL(&state_mux);
    bool current = watch_page == ROOM_WATCH_HOME && !diagnostics_open && object_visible &&
        epoch == current_reader_context.epoch && watch_home_action_locked(reader_current) == painted_home_action;
    if (code == LV_EVENT_PRESSED) {
        pressed_home_action = wake || !current ? ROOM_HOME_NONE : painted_home_action;
        pressed_reader_epoch = pressed_home_action == ROOM_HOME_CONTINUE ? painted_reader_epoch : 0;
    } else {
        if (code == LV_EVENT_SHORT_CLICKED && current && pressed_home_action == painted_home_action &&
            (pressed_home_action != ROOM_HOME_CONTINUE ||
             (pressed_reader_epoch == painted_reader_epoch && pressed_reader_epoch == epoch)))
            requested = pressed_home_action;
        pressed_reader_epoch = 0;
        pressed_home_action = ROOM_HOME_NONE;
    }
    taskEXIT_CRITICAL(&state_mux);
    if (wake) { room_ui_show_face_hint(10000); return; }
    if (requested == ROOM_HOME_CONTINUE && room_conversations_resume_current(epoch))
        (void)watch_navigation_show_context(ROOM_WATCH_CONVERSATIONS, epoch);
    else if (requested == ROOM_HOME_AGENTS) (void)watch_navigation_show(ROOM_WATCH_AGENTS);
    else if (requested == ROOM_HOME_CONNECTION) (void)watch_navigation_show(ROOM_WATCH_CONNECTION);
}

static void watch_navigation_clicked(lv_event_t *event)
{
    room_watch_page_t page = (room_watch_page_t)(intptr_t)lv_event_get_user_data(event);
    if (page == ROOM_WATCH_BACK) {
        taskENTER_CRITICAL(&state_mux);
        page = watch_parent(watch_page);
        taskEXIT_CRITICAL(&state_mux);
    }
    watch_navigation_input(event, page);
}

static void watch_agents_clicked(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESS_LOST && code != LV_EVENT_SHORT_CLICKED) return;
    int action = (int)(intptr_t)lv_event_get_user_data(event);
    bool wake = code == LV_EVENT_PRESSED && room_board_display_is_dark();
    bool activate = false;
    taskENTER_CRITICAL(&state_mux);
    bool current = watch_page == ROOM_WATCH_AGENTS && !diagnostics_open && !conversations_open &&
        current_agents.revision == painted_agents.revision && current_agents.state == painted_agents.state;
    if (code == LV_EVENT_PRESSED) {
        pressed_agent_action = current && !wake ? action : -100;
        pressed_agent_revision = current_agents.revision;
        pressed_agent_state = current_agents.state;
    } else {
        activate = code == LV_EVENT_SHORT_CLICKED && current && pressed_agent_action == action &&
            pressed_agent_revision == current_agents.revision && pressed_agent_state == current_agents.state;
        pressed_agent_action = -100;
    }
    taskEXIT_CRITICAL(&state_mux);
    if (wake) { room_ui_show_face_hint(10000); return; }
    if (!activate) return;
    size_t offset = 0;
    room_agent_action_t result = room_watch_agents_activate(action, &offset);
    if ((result == ROOM_AGENT_SELECT_TARGET || result == ROOM_AGENT_RESET_TARGET) && agent_selection_handler != NULL) {
        char id[65] = {0};
        unsigned source_revision = 0;
        if (result == ROOM_AGENT_RESET_TARGET || room_watch_agents_target(id, &source_revision)) {
            taskENTER_CRITICAL(&state_mux);
            bool submit = !selection_pending;
            if (submit) {
                selection_pending = true;
                selection_failed = false;
                pressed_talk_action = TALK_CONTROL_NONE;
            }
            taskEXIT_CRITICAL(&state_mux);
            if (submit) agent_selection_handler(id, source_revision);
        }
    }
    if (result == ROOM_AGENT_OPEN_CONVERSATIONS) {
        char id[65];
        unsigned revision;
        if (room_watch_agents_target(id, &revision)) open_conversations(id, revision);
    }
    if (result == ROOM_AGENT_LEAVE) (void)watch_navigation_show(ROOM_WATCH_MENU);
    else {
        if (result == ROOM_AGENT_LOAD) request_agents_page(offset);
        room_ui_refresh();
    }
}

static void watch_devices_clicked(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESS_LOST && code != LV_EVENT_SHORT_CLICKED) return;
    int action = (int)(intptr_t)lv_event_get_user_data(event);
    bool wake = code == LV_EVENT_PRESSED && room_board_display_is_dark();
    taskENTER_CRITICAL(&state_mux);
    bool current = watch_page == ROOM_WATCH_DEVICES && !diagnostics_open;
    if (current_devices != NULL && painted_devices != NULL)
        current = current && current_devices->revision == painted_devices->revision &&
            current_devices->state == painted_devices->state;
    unsigned revision = current_devices != NULL ? current_devices->revision : 0;
    esp_openclaw_devices_state_t state = current_devices != NULL ? current_devices->state : ESP_OPENCLAW_DEVICES_ERROR;
    bool activate = false;
    if (code == LV_EVENT_PRESSED) {
        pressed_device_action = current && !wake ? action : -100;
        pressed_device_revision = revision;
        pressed_device_state = state;
    } else {
        activate = code == LV_EVENT_SHORT_CLICKED && current && pressed_device_action == action &&
            pressed_device_revision == revision && pressed_device_state == state;
        pressed_device_action = -100;
    }
    taskEXIT_CRITICAL(&state_mux);
    if (wake) { room_ui_show_face_hint(10000); return; }
    if (!activate) return;
    room_device_action_t result = room_watch_devices_activate(action);
    if (result == ROOM_DEVICE_LEAVE) (void)watch_navigation_show(ROOM_WATCH_MENU);
    else if (result == ROOM_DEVICE_OPEN_LOCAL) (void)watch_navigation_show(ROOM_WATCH_LOCAL_DEVICE);
    else {
        if (result == ROOM_DEVICE_RELOAD) open_devices();
        room_ui_refresh();
    }
}

static void watch_conversations_clicked(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESS_LOST && code != LV_EVENT_SHORT_CLICKED) return;
    int action = (int)(intptr_t)lv_event_get_user_data(event);
    bool wake = code == LV_EVENT_PRESSED && room_board_display_is_dark();
    bool mode_current = room_watch_conversations_input_current();
    taskENTER_CRITICAL(&state_mux);
    bool current = mode_current && conversations_open && watch_page == ROOM_WATCH_AGENTS && !diagnostics_open;
    if (current_conversations != NULL && painted_conversations != NULL) {
        current = current && current_conversations->revision == painted_conversations->revision &&
            current_conversations->preview_revision == painted_conversations->preview_revision &&
            current_conversations->selected == painted_conversations->selected &&
            current_conversations->state == painted_conversations->state &&
            current_conversations->preview_state == painted_conversations->preview_state &&
            memcmp(&current_conversations->creation, &painted_conversations->creation,
                sizeof(current_conversations->creation)) == 0;
    }
    bool activate = code == LV_EVENT_SHORT_CLICKED && current && pressed_chat_action == action;
    if (code == LV_EVENT_PRESSED) pressed_chat_action = current && !wake ? action : -100;
    else pressed_chat_action = -100;
    unsigned revision = current_conversations != NULL ? current_conversations->revision : 0;
    unsigned creation_revision = current_conversations != NULL ? current_conversations->creation.revision : 0;
    taskEXIT_CRITICAL(&state_mux);
    if (wake) { room_ui_show_face_hint(10000); return; }
    if (!activate) return;
    size_t value = 0;
    room_chat_action_t result = room_watch_conversations_activate(action, &value);
    if (result == ROOM_CHAT_LEAVE) close_conversations(true);
    else if (conversation_handler != NULL && (result == ROOM_CHAT_LOAD || result == ROOM_CHAT_READ)) {
        taskENTER_CRITICAL(&state_mux);
        if (current_conversations != NULL) {
            /* Fence publication from before this pending request. */
            ++conversation_generation;
            if (result == ROOM_CHAT_LOAD) current_conversations->state = ROOM_AGENTS_LOADING;
            else {
                current_conversations->selected = value;
                current_conversations->preview_state = ROOM_AGENTS_LOADING;
            }
        }
        taskEXIT_CRITICAL(&state_mux);
        conversation_handler(result == ROOM_CHAT_LOAD ? ROOM_CONVERSATIONS_LOAD : ROOM_CONVERSATIONS_READ,
            "", revision, value, conversation_generation);
    } else if (conversation_handler != NULL && result >= ROOM_CHAT_NEW) {
        room_ui_conversation_action_t intent = result == ROOM_CHAT_NEW ? ROOM_CONVERSATIONS_NEW
            : result == ROOM_CHAT_CREATE ? ROOM_CONVERSATIONS_CREATE
            : result == ROOM_CHAT_OPEN_CREATED ? ROOM_CONVERSATIONS_OPEN_CREATED
            : ROOM_CONVERSATIONS_DISMISS_CREATION;
        conversation_handler(intent, "", result == ROOM_CHAT_NEW ? revision : creation_revision,
            0, conversation_generation);
    }
    room_ui_refresh();
}

static talk_control_action_t talk_control_action(room_ui_talk_state_t talk)
{
    if (talk == ROOM_UI_TALK_READY) return selection_pending || current_target.expired
        ? TALK_CONTROL_NONE : TALK_CONTROL_START;
    if (talk == ROOM_UI_TALK_CONNECTING || talk == ROOM_UI_TALK_ACTIVE) return TALK_CONTROL_STOP;
    return TALK_CONTROL_NONE;
}

void room_ui_set_talk_action_handler(void (*handler)(bool start, uint64_t recipient_revision))
{
    talk_action_handler = handler;
}

void room_ui_set_voice_action_handler(void (*handler)(room_voice_action_t, uint32_t, uint64_t, uint64_t))
{
    voice_action_handler = handler;
}

void room_ui_store_voice(const room_voice_snapshot_t *snapshot)
{
    taskENTER_CRITICAL(&state_mux);
    bool failed = (snapshot->state == ROOM_VOICE_ERROR || snapshot->state == ROOM_VOICE_TRANSCRIPTION_ERROR) &&
        snapshot->state != current_voice.state;
    current_voice = *snapshot;
    taskEXIT_CRITICAL(&state_mux);
    if (watch_home_enabled && failed) start_text_hint(10000);
}

static void dismiss_voice(bool explicit)
{
    /* A release can precede the worker's first repaint. Keep the gesture's
     * generation so covering Home also cancels that queued recording. */
    uint32_t generation = voice_generation > painted_voice.generation ? voice_generation : painted_voice.generation;
    voice_held = voice_started = false;
    discard_pressed_generation = 0;
    send_pressed_generation = 0;
    /* Covering Home must not lose an accepted or uncertain submission. Done
     * releases its local view without sending an abort to the Gateway. */
    taskENTER_CRITICAL(&state_mux);
    bool submitted = current_voice.state >= ROOM_VOICE_SENDING;
    taskEXIT_CRITICAL(&state_mux);
    if (!explicit && (submitted || painted_voice.state >= ROOM_VOICE_SENDING)) return;
    if (generation != 0 && (explicit || generation != voice_canceled_generation) && voice_action_handler != NULL) {
        voice_canceled_generation = generation;
        voice_action_handler(explicit ? ROOM_VOICE_CANCEL : ROOM_VOICE_HIDE, generation, 0, 0);
    }
}

static void cancel_voice(void)
{
    dismiss_voice(false);
}

static bool voice_intent_current(void)
{
    taskENTER_CRITICAL(&state_mux);
    bool current = !diagnostics_open && watch_page == ROOM_WATCH_HOME && !selection_pending &&
        current_voice.generation == painted_voice.generation && current_voice.state == painted_voice.state &&
        current_voice.recipient_revision == painted_voice.recipient_revision &&
        current_voice.owner_incarnation == painted_voice.owner_incarnation;
    if (painted_voice.state == ROOM_VOICE_DRAFT)
        current = current && current_voice.can_send && painted_voice.can_send &&
            !current_target.expired && current_target.revision == painted_target_revision &&
            current_facts.talk == ROOM_UI_TALK_READY;
    taskEXIT_CRITICAL(&state_mux);
    return current && !voice_held && !room_canvas_is_active();
}

static void watch_voice_send(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        bool wake = room_board_display_is_dark();
        send_pressed_generation = !wake && voice_intent_current() &&
            (painted_voice.state == ROOM_VOICE_DRAFT || painted_voice.state > ROOM_VOICE_SENDING)
            ? painted_voice.generation : 0;
        send_pressed_state = painted_voice.state;
        if (wake || send_pressed_generation != 0) room_ui_show_face_hint(10000);
    } else if (code == LV_EVENT_SHORT_CLICKED) {
        bool valid = send_pressed_generation != 0 && send_pressed_generation == painted_voice.generation &&
            send_pressed_state == painted_voice.state && voice_intent_current();
        send_pressed_generation = 0;
        if (!valid) return;
        if (painted_voice.state == ROOM_VOICE_DRAFT) {
            if (voice_action_handler != NULL)
                voice_action_handler(ROOM_VOICE_SEND, painted_voice.generation,
                    painted_voice.recipient_revision, painted_voice.owner_incarnation);
        } else {
            char agent[sizeof(voice_conversation_agent)];
            strlcpy(agent, painted_voice.agent_id, sizeof(agent));
            if (watch_navigation_show(ROOM_WATCH_AGENTS) == ESP_OK) {
                strlcpy(voice_conversation_agent, agent, sizeof(voice_conversation_agent));
                room_ui_refresh();
            }
        }
    } else if (code == LV_EVENT_PRESS_LOST) send_pressed_generation = 0;
}

static void watch_voice_clicked(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED &&
        code != LV_EVENT_PRESS_LOST) return;
    if (code == LV_EVENT_PRESS_LOST) { cancel_voice(); return; }
    bool wake = code == LV_EVENT_PRESSED && room_board_display_is_dark();
    bool visible = lv_obj_is_visible(lv_event_get_current_target(event));
    taskENTER_CRITICAL(&state_mux);
    bool current = visible && !diagnostics_open && watch_page == ROOM_WATCH_HOME && !selection_pending &&
        !current_target.expired && current_target.revision == painted_target_revision &&
        current_facts.talk == ROOM_UI_TALK_READY &&
        current_voice.owner_incarnation == painted_voice.owner_incarnation &&
        current_voice.recipient_revision == painted_voice.recipient_revision &&
        current_voice.generation == painted_voice.generation;
    taskEXIT_CRITICAL(&state_mux);
    if (code == LV_EVENT_PRESSED) {
        if (wake) { room_ui_show_face_hint(10000); return; }
        if (!current || voice_held || voice_generation == UINT32_MAX ||
            (painted_voice.state != ROOM_VOICE_READY && painted_voice.state != ROOM_VOICE_DRAFT &&
             painted_voice.state != ROOM_VOICE_ERROR && painted_voice.state != ROOM_VOICE_TRANSCRIPTION_ERROR)) return;
        lv_indev_t *input = lv_indev_active();
        if (input == NULL) return;
        lv_point_t point;
        lv_indev_get_point(input, &point);
        voice_start_y = point.y;
        voice_pressed_at = lv_tick_get();
        voice_revision = painted_voice.recipient_revision;
        voice_owner = painted_voice.owner_incarnation;
        voice_held = true;
        voice_started = false;
        room_ui_show_face_hint(10000);
        return;
    }
    if (!voice_held) return;
    if (!current || room_canvas_is_active()) { cancel_voice(); return; }
    if (code == LV_EVENT_PRESSING) {
        lv_point_t point;
        lv_indev_get_point(lv_indev_active(), &point);
        if (voice_start_y - point.y >= 48) { cancel_voice(); return; }
        if (!voice_started && lv_tick_elaps(voice_pressed_at) >= 250) {
            ++voice_generation;
            voice_started = true;
            if (voice_action_handler != NULL)
                voice_action_handler(ROOM_VOICE_BEGIN, voice_generation, voice_revision, voice_owner);
        }
    } else if (code == LV_EVENT_RELEASED) {
        bool finish = voice_started && (painted_voice.generation != voice_generation ||
            painted_voice.state == ROOM_VOICE_STARTING || painted_voice.state == ROOM_VOICE_RECORDING);
        voice_held = voice_started = false;
        if (finish && voice_action_handler != NULL)
            voice_action_handler(ROOM_VOICE_FINISH, voice_generation, voice_revision, voice_owner);
        room_ui_refresh();
    }
}

static void watch_voice_discard(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        bool wake = room_board_display_is_dark();
        discard_pressed_generation = !wake && !voice_held && (painted_voice.state == ROOM_VOICE_DRAFT ||
            painted_voice.state == ROOM_VOICE_TRANSCRIBING || painted_voice.state == ROOM_VOICE_TRANSCRIPTION_ERROR ||
            painted_voice.state >= ROOM_VOICE_SENDING)
            ? painted_voice.generation : 0;
        if (wake || discard_pressed_generation != 0) room_ui_show_face_hint(10000);
    } else if (code == LV_EVENT_SHORT_CLICKED) {
        if (discard_pressed_generation != 0 && discard_pressed_generation == painted_voice.generation)
            dismiss_voice(true);
        discard_pressed_generation = 0;
    } else if (code == LV_EVENT_PRESS_LOST) discard_pressed_generation = 0;
}

static void talk_control_clicked(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESS_LOST && code != LV_EVENT_SHORT_CLICKED) return;
    bool wake = code == LV_EVENT_PRESSED && room_board_display_is_dark();
    talk_control_action_t requested = TALK_CONTROL_NONE;
    uint64_t recipient_revision = 0;
    taskENTER_CRITICAL(&state_mux);
    talk_control_action_t action = talk_control_action(current_facts.talk);
    if (action == TALK_CONTROL_STOP && painted_talk_action == TALK_CONTROL_RETURN)
        action = TALK_CONTROL_RETURN;
    if (code == LV_EVENT_PRESSED) {
        bool target_current = action != TALK_CONTROL_START || current_target.revision == painted_target_revision;
        pressed_talk_action = !wake && !diagnostics_open && target_current && action == painted_talk_action
            ? action : TALK_CONTROL_NONE;
    } else {
        if (code == LV_EVENT_SHORT_CLICKED && !diagnostics_open && pressed_talk_action == action) {
            requested = pressed_talk_action;
            if (requested == TALK_CONTROL_START) recipient_revision = painted_target_revision;
        }
        pressed_talk_action = TALK_CONTROL_NONE;
    }
    taskEXIT_CRITICAL(&state_mux);
    if (wake) {
        room_ui_show_face_hint(10000);
    } else if (requested == TALK_CONTROL_RETURN) {
        (void)watch_navigation_show(ROOM_WATCH_CALL);
    } else if (talk_action_handler != NULL && requested != TALK_CONTROL_NONE) {
        talk_action_handler(requested == TALK_CONTROL_START, recipient_revision);
    }
}

extern const uint8_t home_lobster_pixels[] asm("_binary_openclaw_lobster_argb8888_start");
static const lv_image_dsc_t home_lobster = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_ARGB8888,
        .w = 180,
        .h = 180,
        .stride = 180 * 4,
    },
    .data_size = 180 * 180 * 4,
    .data = home_lobster_pixels,
};

static lv_obj_t *home_label(const char *text, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(home);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text_static(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return label;
}

static void home_create(lv_display_t *display, const esp_openclaw_room_node_config_t *board)
{
    int width = lv_display_get_horizontal_resolution(display) - 2 * board->display.safe_inset;
    home = lv_obj_create(lv_screen_active());
    lv_obj_set_width(home, width < 600 ? width : 600);
    lv_obj_set_height(home, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(home, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(home, 0, 0);
    lv_obj_set_style_radius(home, 0, 0);
    lv_obj_set_style_pad_all(home, 0, 0);
    lv_obj_set_style_pad_row(home, 12, 0);
    lv_obj_set_flex_flow(home, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(home, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(home, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(home);

    lv_obj_t *image = lv_image_create(home);
    lv_image_set_src(image, &home_lobster);
    lv_obj_clear_flag(image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
#if LV_FONT_MONTSERRAT_28
    home_label("OpenClaw Room Node", &lv_font_montserrat_28, 0xffffff);
#else
    home_label("OpenClaw Room Node", &lv_font_montserrat_14, 0xffffff);
#endif
    home_label(board->display_name != NULL ? board->display_name : "Room node",
        &lv_font_montserrat_14, 0x9aabaa);
#if LV_FONT_MONTSERRAT_20
    const lv_font_t *font = &lv_font_montserrat_20;
#else
    const lv_font_t *font = &lv_font_montserrat_14;
#endif
    home_wifi = home_label("Wi-Fi  Starting", font, 0x9aabaa);
    home_gateway = home_label("Gateway  Starting", font, 0x9aabaa);
    home_talk = home_label("Talk  Starting", font, 0x9aabaa);
    home_detail = home_label("", &lv_font_montserrat_14, 0x9aabaa);
    lv_obj_set_height(home_detail, 48);
}

static void home_render(const room_ui_facts_t *facts, room_ui_state_t state, const char *detail)
{
    static const char *wifi[] = {
        "Wi-Fi  Starting", "Wi-Fi  Not configured", "Wi-Fi  Connecting",
        "Wi-Fi  Connected", "Wi-Fi  Offline", "Wi-Fi  Unavailable",
    };
    static const char *gateway[] = {
        "Gateway  Starting", "Gateway  Connecting", "Gateway  Connected",
        "Gateway  Offline", "Gateway  Pairing required",
    };
    static const char *talk[] = {
        "Talk  Starting", "Talk  Unavailable", "Talk  Waiting for operator",
        "Talk  Operator session required", "Talk  Ready", "Talk  Connecting",
        "Talk  Active", "Talk  Stopping",
    };
    lv_label_set_text_static(home_wifi,
        (unsigned)facts->wifi < sizeof(wifi) / sizeof(*wifi) ? wifi[facts->wifi] : "Wi-Fi  Unknown");
    lv_label_set_text_static(home_gateway,
        (unsigned)facts->gateway < sizeof(gateway) / sizeof(*gateway) ? gateway[facts->gateway] : "Gateway  Unknown");
    lv_label_set_text_static(home_talk,
        (unsigned)facts->talk < sizeof(talk) / sizeof(*talk) ? talk[facts->talk] : "Talk  Unknown");
    lv_obj_set_style_text_color(home_wifi,
        lv_color_hex(facts->wifi == ROOM_UI_WIFI_CONNECTED ? 0x54d6af : 0x9aabaa), 0);
    lv_obj_set_style_text_color(home_gateway,
        lv_color_hex(facts->gateway == ROOM_UI_GATEWAY_CONNECTED ? 0x54d6af : 0x9aabaa), 0);
    lv_obj_set_style_text_color(home_talk,
        lv_color_hex(facts->talk == ROOM_UI_TALK_ACTIVE ? 0xf4c16b
            : facts->talk == ROOM_UI_TALK_READY ? 0x54d6af : 0x9aabaa), 0);
    const char *text = state == ROOM_UI_ERROR ? (detail[0] != '\0' ? detail : "Error")
        : state == ROOM_UI_SETUP ? detail : "";
    lv_obj_set_style_text_color(home_detail,
        lv_color_hex(state == ROOM_UI_ERROR ? 0xff8383 : 0x9aabaa), 0);
    if (strcmp(lv_label_get_text(home_detail), text) != 0) {
        lv_label_set_text(home_detail, text);
    }
}

static void repaint_retry_expired(void *arg);

static void text_hint_expired(void *arg)
{
    (void)arg;
    taskENTER_CRITICAL(&state_mux);
    text_hint_active = false;
    taskEXIT_CRITICAL(&state_mux);
    room_ui_refresh();
}

static void start_text_hint(uint32_t show_ms)
{
    taskENTER_CRITICAL(&state_mux);
    text_hint_active = true;
    taskEXIT_CRITICAL(&state_mux);
    if (text_hint_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = text_hint_expired,
            .name = "text_hint",
        };
        (void)esp_timer_create(&args, &text_hint_timer);
    }
    bool timer_started = false;
    if (text_hint_timer != NULL) {
        (void)esp_timer_stop(text_hint_timer);
        timer_started = esp_timer_start_once(
            text_hint_timer, (uint64_t)show_ms * 1000U) == ESP_OK;
    }
    if (!timer_started) {
        taskENTER_CRITICAL(&state_mux);
        text_hint_active = false;
        taskEXIT_CRITICAL(&state_mux);
    }
}

static bool controller_canvas_active(void)
{
    return room_canvas_is_active();
}

static void controller_canvas_action(room_canvas_action_t action, uint32_t value)
{
    if (action == ROOM_CANVAS_ACTION_REQUEST_FACE_HINT) {
        room_ui_show_face_hint(value);
    } else {
        room_ui_refresh();
    }
}

/* One-shot retry: a failed display-lock paint self-heals shortly after. */
static void arm_repaint_retry(void)
{
    static esp_timer_handle_t retry_timer;
    if (retry_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = repaint_retry_expired,
            .name = "ui_repaint",
        };
        if (esp_timer_create(&args, &retry_timer) != ESP_OK) {
            return;
        }
    }
    esp_timer_stop(retry_timer);
    esp_timer_start_once(retry_timer, 40000);
}

static void repaint_retry_expired(void *arg)
{
    (void)arg;
    room_ui_refresh();
}

void room_ui_init(void)
{
    const room_face_controller_t face_controller = {
        .refresh = room_ui_refresh,
        .show_hint = room_ui_show_face_hint,
        .talk_active = room_ui_talk_face_active,
        .canvas_active = controller_canvas_active,
    };
    room_face_set_controller(&face_controller);
    room_canvas_set_action_handler(controller_canvas_action);
    lv_display_t *display = room_board_display_start();
    if (display == NULL) {
        ESP_LOGE(TAG, "failed to start display");
        return;
    }
    if (!room_board_display_lock(0)) {
        ESP_LOGE(TAG, "failed to lock display during initialization");
        return;
    }
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
    status_label = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_color(status_label, lv_color_white(), 0);
    /* Multi-line status text centers on the round glass no matter which path
     * painted it; previously alignment depended on whether the awake hint ran. */
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
#if LV_FONT_MONTSERRAT_28
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_28, 0);
#else
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_14, 0);
#endif
    lv_obj_center(status_label);
    lv_label_set_text(status_label, "OpenClaw");
    /* Small always-available gateway line at the bottom of the glass; shown
     * alongside the face so a glance answers "which claw is this?". */
    gateway_label = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_color(gateway_label, lv_color_hex(0x8a8a8a), 0);
    lv_obj_set_style_text_font(gateway_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(gateway_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(gateway_label, LV_ALIGN_BOTTOM_MID, 0, -56);
    lv_label_set_text(gateway_label, "");
    lv_obj_add_flag(gateway_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(
        lv_screen_active(),
        room_ui_status_clicked,
        LV_EVENT_ALL,
        NULL);
    const esp_openclaw_room_node_config_t *board = room_board_config();
    watch_home_enabled = board != NULL && board->model_identifier != NULL &&
        strcmp(board->model_identifier, "waveshare-esp32-s3-touch-amoled-1.8") == 0;
    if (watch_home_enabled) {
        lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x15101b), 0);
        lv_obj_set_style_bg_grad_color(lv_screen_active(), lv_color_hex(0x362031), 0);
        lv_obj_set_style_bg_grad_dir(lv_screen_active(), LV_GRAD_DIR_VER, 0);
        room_watch_home_create(lv_layer_top(), watch_home_menu_clicked, watch_home_agent_clicked,
            watch_home_continue_clicked, watch_voice_clicked, watch_voice_discard, watch_voice_send);
        room_watch_navigation_create(lv_layer_top(), watch_navigation_clicked);
        room_watch_settings_create(lv_layer_top(), watch_navigation_clicked, watch_messaging_clicked);
        room_watch_agents_create(lv_layer_top(), watch_agents_clicked);
        room_watch_devices_create(lv_layer_top(), watch_devices_clicked);
        room_watch_conversations_create(lv_layer_top(), watch_conversations_clicked);
        room_watch_status_create(lv_layer_top());
    }
    bool board_has_animated_face = board != NULL && board->display.animated_face;
    idle_brightness = board != NULL ? board->display.idle_brightness : 0;
    animated_face_enabled = board_has_animated_face;
    if (animated_face_enabled && room_face_create(lv_screen_active()) != ESP_OK) {
        animated_face_enabled = false;
        ESP_LOGW(TAG, "face unavailable; talk states fall back to text");
    }
    if (board != NULL && !board_has_animated_face) home_create(display, board);
    if (talk_action_handler != NULL) {
        talk_pill = lv_obj_create(lv_layer_top());
        lv_obj_set_size(talk_pill, 148, 44);
        lv_obj_align(talk_pill, LV_ALIGN_TOP_MID, 0, 14);
        lv_obj_set_style_bg_color(talk_pill, lv_color_hex(0x181818), 0);
        lv_obj_set_style_bg_opa(talk_pill, LV_OPA_90, 0);
        lv_obj_set_style_border_width(talk_pill, 0, 0);
        lv_obj_set_style_radius(talk_pill, 12, 0);
        lv_obj_set_style_pad_all(talk_pill, 0, 0);
        if (watch_home_enabled) {
            lv_obj_set_size(talk_pill, 316, 52);
            lv_obj_align(talk_pill, LV_ALIGN_BOTTOM_MID, 0, -20);
            lv_obj_set_style_bg_color(talk_pill, lv_color_hex(0xf4efee), 0);
            lv_obj_set_style_bg_opa(talk_pill, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(talk_pill, LV_RADIUS_CIRCLE, 0);
        }
        lv_obj_clear_flag(talk_pill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(talk_pill, talk_control_clicked, LV_EVENT_ALL, NULL);
        talk_pill_label = lv_label_create(talk_pill);
        lv_obj_set_style_text_color(talk_pill_label, lv_color_white(), 0);
        if (watch_home_enabled) {
            lv_obj_set_style_text_color(talk_pill_label, lv_color_hex(0x302131), 0);
        }
        lv_obj_set_style_text_font(talk_pill_label, &lv_font_montserrat_14, 0);
        lv_obj_clear_flag(talk_pill_label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(talk_pill_label);
        if (watch_home_enabled) {
            talk_target_label = lv_label_create(talk_pill);
            lv_obj_set_size(talk_target_label, 280, 18);
            lv_label_set_long_mode(talk_target_label, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_font(talk_target_label, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_align(talk_target_label, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_color(talk_target_label, lv_color_hex(0x66596a), 0);
            lv_obj_set_clickable(talk_target_label, false);
            lv_obj_align(talk_target_label, LV_ALIGN_TOP_MID, 0, 29);
            lv_obj_set_hidden(talk_target_label, true);
        }
    }
    room_board_display_unlock();
    room_ui_refresh();
}

/* Talk-driven states render as the animated face; text is for setup/errors. */
static bool room_ui_state_uses_face(room_ui_state_t state)
{
    return animated_face_enabled &&
        (state == ROOM_UI_LISTENING || state == ROOM_UI_CONNECTING ||
         state == ROOM_UI_SPEAKING);
}

static int room_ui_target_brightness(room_ui_state_t state, bool canvas_active)
{
    taskENTER_CRITICAL(&state_mux);
    bool overlay = diagnostics_open;
    bool text_hint = text_hint_active;
    bool camera = camera_indicator_active;
    bool navigation = watch_home_enabled && watch_page != ROOM_WATCH_HOME;
    bool voice = watch_home_enabled && current_voice.state >= ROOM_VOICE_STARTING &&
        current_voice.state <= ROOM_VOICE_DRAFT;
    taskEXIT_CRITICAL(&state_mux);
    int idle_level = text_hint ? (watch_home_enabled ? 40 : 18) : idle_brightness;
    int brightness = overlay || canvas_active || navigation || voice ? ROOM_CANVAS_ACTIVE_BRIGHTNESS
        : state == ROOM_UI_IDLE ? idle_level
        : room_ui_state_uses_face(state) ? 40 : 18;
    return camera && brightness < ROOM_UI_CAMERA_MIN_BRIGHTNESS
        ? ROOM_UI_CAMERA_MIN_BRIGHTNESS : brightness;
}

static int watch_display_level(int requested, int preference, bool camera)
{
    int level = requested > 0 && preference >= 0 ? preference : requested;
    return camera && level < ROOM_UI_CAMERA_MIN_BRIGHTNESS ? ROOM_UI_CAMERA_MIN_BRIGHTNESS : level;
}

esp_err_t room_ui_apply_display_brightness(int percent)
{
    if (!watch_home_enabled) return room_board_display_brightness_set(percent);
    // Serialize the preference snapshot and hardware write with setting changes.
    // The Watch port uses the recursive LVGL lock, including in Canvas callbacks.
    if (!room_board_display_lock(100)) return ESP_ERR_TIMEOUT;
    taskENTER_CRITICAL(&state_mux);
    int preference = watch_brightness;
    bool camera = camera_indicator_active;
    taskEXIT_CRITICAL(&state_mux);
    int level = watch_display_level(percent, preference, camera);
    esp_err_t err = room_board_display_brightness_set(level);
    if (err == ESP_OK) {
        room_watch_status_set_awake(level > 0);
        room_watch_navigation_set_awake(level > 0);
    }
    room_board_display_unlock();
    return err;
}

esp_err_t room_ui_watch_brightness(int percent)
{
    if (percent < 10 || percent > 100) return ESP_ERR_INVALID_ARG;
    if (!watch_home_enabled || status_label == NULL) return ESP_ERR_INVALID_STATE;
    if (!room_board_display_lock(100)) {
        taskENTER_CRITICAL(&state_mux);
        watch_brightness_failed = true;
        pressed_watch_page = -1;
        taskEXIT_CRITICAL(&state_mux);
        arm_repaint_retry();
        return ESP_ERR_TIMEOUT;
    }
    taskENTER_CRITICAL(&state_mux);
    room_ui_state_t state = current_state;
    bool camera = camera_indicator_active;
    pressed_watch_page = -1;
    taskEXIT_CRITICAL(&state_mux);
    int requested = room_ui_target_brightness(state, room_canvas_is_active());
    int level = watch_display_level(requested, percent, camera);
    esp_err_t err = room_board_display_brightness_set(level);
    if (err == ESP_OK) {
        room_watch_status_set_awake(level > 0);
        room_watch_navigation_set_awake(level > 0);
    }
    taskENTER_CRITICAL(&state_mux);
    if (err == ESP_OK) watch_brightness = percent;
    watch_brightness_failed = err != ESP_OK;
    taskEXIT_CRITICAL(&state_mux);
    room_ui_paint_and_unlock();
    return err;
}

/* Paints `current_state`/`current_detail` with the display lock held. Returns
 * true when the caller must apply the idle/active brightness after unlocking. */
static bool room_ui_render_locked(void)
{
    static const char *labels[] = {"OpenClaw", "Listening", "Connecting", "Speaking", "Error", "Setup"};
    /* Snapshot the facts under their own lock; the writer may not hold ours. */
    taskENTER_CRITICAL(&state_mux);
    room_ui_state_t state = current_state;
    char detail[sizeof(current_detail)];
    memcpy(detail, current_detail, sizeof(detail));
    char gateway[sizeof(gateway_text)];
    memcpy(gateway, gateway_text, sizeof(gateway));
    bool overlay = diagnostics_open;
    room_ui_facts_t facts = current_facts;
    room_ui_talk_target_t target = current_target;
    static room_voice_snapshot_t voice;
    voice = current_voice;
    room_conversation_context_t reader_context = current_reader_context;
    room_home_action_t home_action = watch_home_action_locked(reader_context.epoch != 0);
    bool selecting = selection_pending, selection_error = selection_failed;
    room_messaging_snapshot_t messaging = current_messaging;
    if (messaging_pending || messaging_handler == NULL) messaging.can_request = false;
    painted_messaging = messaging;
    bool permission_busy = messaging_pending || messaging.busy;
    if (permission_busy && facts.talk == ROOM_UI_TALK_READY) facts.talk = ROOM_UI_TALK_WAITING;
    room_watch_page_t page = watch_page;
    room_watch_page_t parent = watch_parent(page);
    int brightness = watch_brightness < 0 ? ROOM_CANVAS_ACTIVE_BRIGHTNESS : watch_brightness;
    bool brightness_failed = watch_brightness_failed;
    bool camera_active = camera_indicator_active;
    bool camera_minimum = camera_active && brightness < ROOM_UI_CAMERA_MIN_BRIGHTNESS;
    bool agents_changed = current_agents.revision != painted_agents.revision || current_agents.state != painted_agents.state;
    painted_agents = current_agents;
    char open_voice_agent[65] = {0};
    if (page == ROOM_WATCH_AGENTS && voice_conversation_agent[0] != '\0' &&
        current_agents.state != ROOM_AGENTS_LOADING) {
        if (current_agents.state == ROOM_AGENTS_READY) {
            for (size_t i = 0; i < current_agents.page.count; ++i) {
                if (strcmp(current_agents.page.agents[i].id, voice_conversation_agent) == 0) {
                    strlcpy(open_voice_agent, voice_conversation_agent, sizeof(open_voice_agent));
                    break;
                }
            }
        }
        voice_conversation_agent[0] = '\0';
    }
    bool chats = conversations_open;
    bool resume_failed = conversation_resume_failed;
    bool chats_changed = current_conversations != NULL && painted_conversations != NULL &&
        (current_conversations->revision != painted_conversations->revision ||
         current_conversations->preview_revision != painted_conversations->preview_revision ||
         current_conversations->selected != painted_conversations->selected ||
         current_conversations->state != painted_conversations->state ||
         current_conversations->preview_state != painted_conversations->preview_state ||
         memcmp(&current_conversations->creation, &painted_conversations->creation,
             sizeof(current_conversations->creation)) != 0);
    if (chats_changed) *painted_conversations = *current_conversations;
    bool devices_changed = current_devices != NULL && painted_devices != NULL &&
        (current_devices->revision != painted_devices->revision || current_devices->state != painted_devices->state);
    if (devices_changed) *painted_devices = *current_devices;
    taskEXIT_CRITICAL(&state_mux);
    if (open_voice_agent[0] != '\0') {
        open_conversations(open_voice_agent, painted_agents.revision);
        chats = true;
    }
    if (((agents_changed || chats_changed) && page == ROOM_WATCH_AGENTS) ||
        (devices_changed && page == ROOM_WATCH_DEVICES)) {
        for (lv_indev_t *input = lv_indev_get_next(NULL); input != NULL; input = lv_indev_get_next(input)) {
            if (lv_indev_get_display(input) == lv_obj_get_display(status_label) &&
                lv_indev_get_state(input) == LV_INDEV_STATE_PRESSED) {
                lv_indev_reset(input, NULL);
                lv_indev_wait_release(input);
            }
        }
    }
    bool canvas_active = room_canvas_is_active();
    bool navigation = watch_home_enabled && page != ROOM_WATCH_HOME && !canvas_active;
    bool call_page = watch_home_enabled && page == ROOM_WATCH_CALL;
    bool call_in_progress = facts.talk == ROOM_UI_TALK_CONNECTING || facts.talk == ROOM_UI_TALK_ACTIVE;
    // The call page keeps its control; other pages reserve space only during a call.
    int content_bottom = call_in_progress || call_page ? 370 : 432;
    bool voice_home = watch_home_enabled && voice_action_handler != NULL &&
        facts.talk != ROOM_UI_TALK_CONNECTING && facts.talk != ROOM_UI_TALK_ACTIVE &&
        facts.talk != ROOM_UI_TALK_STOPPING;
    if (watch_home_enabled && (canvas_active || overlay || navigation || !voice_home)) cancel_voice();
    painted_voice = voice;
    if (watch_home_enabled) {
        bool current = reader_context.epoch != 0 && room_conversations_resume_current(reader_context.epoch);
        if (home_action == ROOM_HOME_CONTINUE && !current) home_action = ROOM_HOME_AGENTS;
        room_watch_home_render(&facts, &target, state, detail,
            current ? reader_context.agent_name : NULL, voice_home ? &voice : NULL,
            facts.talk == ROOM_UI_TALK_READY && !selecting && !target.expired && voice.state != ROOM_VOICE_UNAVAILABLE,
            voice_held, home_action, canvas_active || overlay || navigation);
        taskENTER_CRITICAL(&state_mux);
        painted_target_revision = target.revision;
        if (painted_home_action != home_action) pressed_home_action = ROOM_HOME_NONE;
        painted_home_action = home_action;
        painted_reader_epoch = home_action == ROOM_HOME_CONTINUE ? reader_context.epoch : 0;
        if (pressed_reader_epoch != painted_reader_epoch) pressed_reader_epoch = 0;
        taskEXIT_CRITICAL(&state_mux);
        bool settings = page == ROOM_WATCH_SETTINGS || page == ROOM_WATCH_LOCAL_DEVICE || page == ROOM_WATCH_DISPLAY || page == ROOM_WATCH_BRIGHTNESS || page == ROOM_WATCH_READING || page == ROOM_WATCH_DENSITY || page == ROOM_WATCH_MESSAGING;
        room_watch_navigation_render(page, &facts, !navigation || overlay || page == ROOM_WATCH_AGENTS ||
            page == ROOM_WATCH_DEVICES || settings, resume_failed, parent, content_bottom);
        room_watch_devices_render(painted_devices, !navigation || overlay || page != ROOM_WATCH_DEVICES, content_bottom);
        room_watch_settings_render(page, parent, brightness, brightness_failed, camera_minimum, &messaging,
            !navigation || overlay || !settings, content_bottom);
        room_watch_agents_render(&painted_agents, !navigation || overlay || page != ROOM_WATCH_AGENTS || chats,
            target.selected ? target.id : "", target.expired, selecting, selection_error, content_bottom);
        room_watch_conversations_render(painted_conversations,
            !navigation || overlay || page != ROOM_WATCH_AGENTS || !chats, content_bottom);
        bool status_page = page == ROOM_WATCH_HOME || page == ROOM_WATCH_MENU ||
            page == ROOM_WATCH_CONNECTION || page == ROOM_WATCH_CALL ||
            page == ROOM_WATCH_AGENTS || page == ROOM_WATCH_DEVICES || settings;
        room_watch_status_render(&facts, !status_page || canvas_active || overlay || camera_active);
    }
    if (talk_pill != NULL) {
        if (watch_home_enabled) {
            /* Retained Canvas content keeps its existing top action clearance. */
            lv_obj_set_size(talk_pill, canvas_active ? 148 : 316, canvas_active ? 44 : 52);
            lv_obj_align(talk_pill, canvas_active ? LV_ALIGN_TOP_MID : LV_ALIGN_BOTTOM_MID,
                0, canvas_active ? 14 : -20);
        }
        painted_talk_action = facts.talk == ROOM_UI_TALK_READY
            ? selecting || target.expired ? TALK_CONTROL_NONE : TALK_CONTROL_START
            : facts.talk == ROOM_UI_TALK_CONNECTING || facts.talk == ROOM_UI_TALK_ACTIVE ? TALK_CONTROL_STOP : TALK_CONTROL_NONE;
        bool start = painted_talk_action == TALK_CONTROL_START;
        bool stop = facts.talk == ROOM_UI_TALK_CONNECTING || facts.talk == ROOM_UI_TALK_ACTIVE;
        bool return_to_call = watch_home_enabled && !call_page && stop;
        if (return_to_call) painted_talk_action = TALK_CONTROL_RETURN;
        const char *start_label = call_page ? "Start call" : "Start Talk";
        const char *stop_label = call_page ? "End call" : "Stop Talk";
        lv_label_set_text(talk_pill_label, return_to_call ? "Return to call" : start ? start_label : stop ? stop_label
            : selecting ? "Choosing agent..." : target.expired ? "Choose an agent" :
            facts.talk == ROOM_UI_TALK_STOPPING ? "Stopping..." : "Talk unavailable");
        if (talk_target_label != NULL) {
            bool show_target = start && !canvas_active;
            lv_obj_set_hidden(talk_target_label, !show_target);
            lv_obj_set_style_text_font(talk_pill_label, &lv_font_montserrat_14, 0);
            if (show_target) {
                char recipient[80];
                snprintf(recipient, sizeof(recipient), "with %s", target.selected ? target.name : "default agent");
                lv_label_set_text(talk_target_label, recipient);
#if LV_FONT_MONTSERRAT_20
                lv_obj_set_style_text_font(talk_pill_label, &lv_font_montserrat_20, 0);
#endif
                lv_obj_align(talk_pill_label, LV_ALIGN_TOP_MID, 0, 3);
            } else {
                lv_obj_center(talk_pill_label);
            }
        }
#if LV_FONT_MONTSERRAT_20
        if (call_page) lv_obj_set_style_text_font(talk_pill_label, &lv_font_montserrat_20, 0);
#endif
        taskENTER_CRITICAL(&state_mux);
        painted_target_revision = target.revision;
        taskEXIT_CRITICAL(&state_mux);
        lv_obj_set_state(talk_pill, LV_STATE_DISABLED, !start && !stop);
        bool browsing_idle = watch_home_enabled && !call_page && !call_in_progress && facts.talk != ROOM_UI_TALK_STOPPING;
        bool hidden = overlay || (voice_home && !canvas_active && !navigation) || browsing_idle;
        if (browsing_idle) {
            painted_talk_action = TALK_CONTROL_NONE;
            pressed_talk_action = TALK_CONTROL_NONE;
        }
        lv_obj_set_flag(talk_pill, LV_OBJ_FLAG_HIDDEN, hidden);
        if (!hidden) lv_obj_move_foreground(talk_pill);
    }
    if (gateway_label != NULL) {
        /* The gateway line rides along with every non-canvas view. */
        lv_label_set_text(gateway_label, gateway);
        if (watch_home_enabled || home != NULL || canvas_active || overlay || gateway[0] == '\0') {
            lv_obj_add_flag(gateway_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(gateway_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(gateway_label);
        }
    }
    if (home != NULL) {
        if (canvas_active || overlay) {
            lv_obj_add_flag(home, LV_OBJ_FLAG_HIDDEN);
        } else {
            home_render(&facts, state, detail);
            lv_obj_clear_flag(home, LV_OBJ_FLAG_HIDDEN);
        }
        if (camera_indicator != NULL) lv_obj_move_foreground(camera_indicator);
    }
    if (canvas_active || navigation || (watch_home_enabled && room_watch_home_review_visible())) {
        room_face_hide();
        lv_obj_set_hidden(status_label, true);
        if (state == ROOM_UI_IDLE) {
            /* A call that ended behind canvas must not leak its mood into the
             * next call. */
            room_face_reset_mood();
        }
        if (camera_indicator != NULL) lv_obj_move_foreground(camera_indicator);
        return true;
    }
    if (home != NULL) {
        room_face_hide();
        room_face_reset_mood();
        lv_obj_add_flag(status_label, LV_OBJ_FLAG_HIDDEN);
        return true;
    }
    if (room_ui_state_uses_face(state)) {
        room_face_show(
            state == ROOM_UI_SPEAKING ? ROOM_FACE_SPEAKING
            : state == ROOM_UI_CONNECTING ? ROOM_FACE_THINKING
                                                  : ROOM_FACE_LISTENING);
        /* If face creation failed at boot, fall through to the text states. */
        if (room_face_is_visible()) {
            lv_obj_add_flag(status_label, LV_OBJ_FLAG_HIDDEN);
            return true;
        }
    }
    room_face_hide();
    /* A talk session (or an expired hint) is over; a call-scoped mood must not
     * leak into the next call. The canvas branch above deliberately keeps the
     * mood because canvas can cover a still-active call. */
    room_face_reset_mood();
    if (watch_home_enabled) {
        room_face_show_home();
        lv_obj_add_flag(status_label, LV_OBJ_FLAG_HIDDEN);
        return true;
    }
    lv_obj_clear_flag(status_label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(
        status_label,
        detail[0] != '\0' ? "%s\n%s" : "%s",
        labels[state],
        detail);
    return true;
}

bool room_ui_talk_face_active(void)
{
    taskENTER_CRITICAL(&state_mux);
    room_ui_state_t state = current_state;
    taskEXIT_CRITICAL(&state_mux);
    return room_ui_state_uses_face(state);
}

/* Precondition: display lock held. Renders, releases the lock, then applies
 * the brightness for the state that was actually painted. */
static void room_ui_paint_and_unlock(void)
{
    bool apply_brightness = room_ui_render_locked();
    taskENTER_CRITICAL(&state_mux);
    room_ui_state_t painted_state = current_state;
    taskEXIT_CRITICAL(&state_mux);
    room_board_display_unlock();
    if (apply_brightness) {
        /* AMOLED is fully dark while idle; the face gets more headroom than
         * the plain text states so expressions read across the room. */
        int brightness = room_ui_target_brightness(
            painted_state,
            room_canvas_is_active());
        room_ui_apply_display_brightness(brightness);
    }
}

void room_ui_set(room_ui_state_t state, const char *detail)
{
    if (status_label == NULL) {
        ESP_LOGE(TAG, "status display is not initialized");
        return;
    }
    /* Record the fact unconditionally; rendering is best-effort below. */
    taskENTER_CRITICAL(&state_mux);
    current_state = state;
    current_detail[0] = '\0';
    if (detail != NULL) {
        strlcpy(current_detail, detail, sizeof(current_detail));
    }
    taskEXIT_CRITICAL(&state_mux);
    if (!room_board_display_lock(100)) {
        ESP_LOGW(TAG, "display busy; state stored, repaint retries");
        arm_repaint_retry();
        return;
    }
    room_ui_paint_and_unlock();
}

void room_ui_store_facts(const room_ui_facts_t *facts)
{
    if (facts == NULL) return;
    taskENTER_CRITICAL(&state_mux);
    if (talk_control_action(current_facts.talk) != talk_control_action(facts->talk)) {
        pressed_talk_action = TALK_CONTROL_NONE;
    }
    current_facts = *facts;
    taskEXIT_CRITICAL(&state_mux);
}

void room_ui_set_gateway(const char *gateway_host)
{
    taskENTER_CRITICAL(&state_mux);
    gateway_text[0] = '\0';
    if (gateway_host != NULL) {
        strlcpy(gateway_text, gateway_host, sizeof(gateway_text));
    }
    taskEXIT_CRITICAL(&state_mux);
    room_ui_refresh();
}

void room_ui_show_face_hint(uint32_t show_ms)
{
    if (status_label == NULL) {
        return;
    }
    /* A Watch wake must survive display contention and static Home repaints. */
    bool watch_hint = watch_home_enabled && !watch_navigation_active();
    if (watch_hint) start_text_hint(show_ms);
    if (!room_board_display_lock(100)) {
        if (watch_hint) arm_repaint_retry();
        return;
    }
    if (watch_navigation_active() || (watch_home_enabled && room_watch_home_review_visible())) {
        room_ui_paint_and_unlock();
        return;
    }
    /* Animated-only boards keep the face-owned lifetime. The Watch's static
     * Home and text fallback use the controller-owned wake timer. */
    room_face_show_hint(esp_timer_get_time() + (int64_t)show_ms * 1000);
    /* If face creation failed at boot the show is a no-op; keep the text
     * status instead of leaving a blank lit panel for the hold duration. */
    bool shown = room_face_is_visible();
    if (shown) {
        lv_obj_add_flag(status_label, LV_OBJ_FLAG_HIDDEN);
        if (gateway_label != NULL && !watch_home_enabled) {
            /* The wake-up face is the "which claw am I?" moment: surface the
             * connected gateway under it whenever one is known. */
            taskENTER_CRITICAL(&state_mux);
            char gateway[sizeof(gateway_text)];
            memcpy(gateway, gateway_text, sizeof(gateway));
            taskEXIT_CRITICAL(&state_mux);
            lv_label_set_text(gateway_label, gateway);
            if (gateway[0] != '\0') {
                lv_obj_clear_flag(gateway_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(gateway_label);
            }
        }
    }
    if (!shown) {
        if (!watch_hint) start_text_hint(show_ms);
        (void)room_ui_render_locked();
    }
    room_board_display_unlock();
    if (shown) {
        room_ui_apply_display_brightness(40);
    }
    if (!shown) room_ui_refresh();
}

void room_ui_refresh(void)
{
    if (status_label == NULL) {
        return;
    }
    if (!room_board_display_lock(100)) {
        arm_repaint_retry();
        return;
    }
    /* Repaint the stored state under one lock hold; a concurrent Talk
     * transition either lands before this render or repaints right after it,
     * so no snapshot of the state can overwrite a newer one. */
    room_ui_paint_and_unlock();
}

esp_err_t room_ui_camera_indicator_begin(void)
{
    if (status_label == NULL || !room_board_display_lock(500)) {
        return ESP_ERR_TIMEOUT;
    }
    if (camera_indicator == NULL) {
        camera_indicator = lv_label_create(lv_layer_top());
    }
    if (camera_indicator == NULL) {
        room_board_display_unlock();
        return ESP_ERR_NO_MEM;
    }
    lv_label_set_text(camera_indicator, LV_SYMBOL_EYE_OPEN " Camera active");
    lv_obj_set_style_text_color(camera_indicator, lv_color_hex(0xff4d4d), 0);
    lv_obj_set_style_bg_color(camera_indicator, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(camera_indicator, LV_OPA_90, 0);
    lv_obj_set_style_pad_all(camera_indicator, 10, 0);
    lv_obj_align(camera_indicator, LV_ALIGN_TOP_RIGHT, -18, 18);
    lv_obj_clear_flag(camera_indicator, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(camera_indicator);
    if (watch_home_enabled) room_watch_status_render(NULL, true);
    taskENTER_CRITICAL(&state_mux);
    camera_indicator_active = true;
    taskEXIT_CRITICAL(&state_mux);
    lv_refr_now(lv_display_get_default());
    room_board_display_unlock();
    taskENTER_CRITICAL(&state_mux);
    room_ui_state_t state = current_state;
    taskEXIT_CRITICAL(&state_mux);
    int brightness = room_ui_target_brightness(state, room_canvas_is_active());
    if (brightness < ROOM_UI_CAMERA_MIN_BRIGHTNESS) {
        brightness = ROOM_UI_CAMERA_MIN_BRIGHTNESS;
    }
    esp_err_t err = room_ui_apply_display_brightness(brightness);
    if (err != ESP_OK) {
        room_ui_camera_indicator_end();
    }
    return err;
}

void room_ui_camera_indicator_end(void)
{
    if (room_board_display_lock(500)) {
        if (camera_indicator != NULL) {
            lv_obj_delete(camera_indicator);
            camera_indicator = NULL;
            taskENTER_CRITICAL(&state_mux);
            camera_indicator_active = false;
            taskEXIT_CRITICAL(&state_mux);
            lv_refr_now(lv_display_get_default());
        }
        room_board_display_unlock();
    }
    /* Repaint restores the controller-owned brightness for the live state. */
    room_ui_refresh();
}

void room_ui_get_diagnostics(room_ui_diagnostics_snapshot_t *snapshot)
{
    if (snapshot == NULL) return;
    taskENTER_CRITICAL(&state_mux);
    snapshot->state = current_state;
    snapshot->diagnostics_open = diagnostics_open;
    memcpy(snapshot->detail, current_detail, sizeof(snapshot->detail));
    memcpy(snapshot->gateway, gateway_text, sizeof(snapshot->gateway));
    taskEXIT_CRITICAL(&state_mux);
}

const char *room_ui_state_name(room_ui_state_t state)
{
    static const char *names[] = {
        "idle", "listening", "connecting", "speaking", "error", "setup",
    };
    return state >= ROOM_UI_IDLE && state <= ROOM_UI_SETUP ? names[state] : "unknown";
}

void room_ui_set_diagnostics_open(bool open)
{
    taskENTER_CRITICAL(&state_mux);
    diagnostics_open = open;
    if (open) {
        pressed_talk_action = TALK_CONTROL_NONE;
        pressed_device_action = -100;
        messaging_held = false;
        pressed_watch_page = -1;
        pressed_reader_epoch = 0;
        painted_reader_epoch = 0;
        pressed_home_action = ROOM_HOME_NONE;
        painted_home_action = ROOM_HOME_NONE;
        if (watch_page == ROOM_WATCH_CONVERSATIONS) {
            watch_page = ROOM_WATCH_MENU;
            ++conversation_generation;
        }
    }
    taskEXIT_CRITICAL(&state_mux);
    if (open) {
        if (watch_home_enabled) {
            cancel_voice();
            room_watch_home_render(NULL, NULL, ROOM_UI_IDLE, "", NULL, NULL, false, false, ROOM_HOME_NONE, true);
            room_watch_status_render(NULL, true);
        }
        if (watch_home_enabled) {
            room_watch_navigation_render(ROOM_WATCH_HOME, NULL, true, false, ROOM_WATCH_HOME, 432);
            room_watch_settings_render(ROOM_WATCH_HOME, ROOM_WATCH_HOME, 0, false, false, NULL, true, 432);
        }
        if (home != NULL) lv_obj_add_flag(home, LV_OBJ_FLAG_HIDDEN);
        if (talk_pill != NULL) lv_obj_add_flag(talk_pill, LV_OBJ_FLAG_HIDDEN);
        if (camera_indicator != NULL) lv_obj_move_foreground(camera_indicator);
        room_ui_apply_display_brightness(ROOM_CANVAS_ACTIVE_BRIGHTNESS);
    } else {
        room_ui_refresh();
    }
}

void room_ui_raise_camera_indicator(void)
{
    if (camera_indicator != NULL) lv_obj_move_foreground(camera_indicator);
}

bool room_ui_is_initialized(void)
{
    return status_label != NULL;
}
