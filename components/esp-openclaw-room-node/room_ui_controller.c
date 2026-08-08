#include "room_ui_controller.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "room_canvas.h"
#include "room_face.h"
#include "room_board.h"

/* Tap on the status screen: agent canvas when present, else wake the face. */
static void room_ui_status_clicked(lv_event_t *event)
{
    (void)event;
    room_canvas_view_toggle();
}

static const char *TAG = "room_ui";
#define ROOM_UI_CAMERA_MIN_BRIGHTNESS 40
static lv_obj_t *status_label;
static lv_obj_t *gateway_label;
static lv_obj_t *talk_pill;
static lv_obj_t *talk_pill_label;
static lv_obj_t *camera_indicator;
static bool animated_face_enabled;
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

static void repaint_retry_expired(void *arg);

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
        LV_EVENT_CLICKED,
        NULL);
    const esp_openclaw_room_node_config_t *board = room_board_config();
    animated_face_enabled = board != NULL && board->display.animated_face;
    if (animated_face_enabled && room_face_create(lv_screen_active()) != ESP_OK) {
        animated_face_enabled = false;
        ESP_LOGW(TAG, "face unavailable; talk states fall back to text");
    }
    room_board_display_unlock();
    room_board_display_brightness_set(0);
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
    if (canvas_active) return ROOM_CANVAS_ACTIVE_BRIGHTNESS;
    if (state == ROOM_UI_IDLE) return 0;
    return room_ui_state_uses_face(state) ? 40 : 18;
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
    taskEXIT_CRITICAL(&state_mux);
    if (gateway_label != NULL) {
        /* The gateway line rides along with every non-canvas view. */
        lv_label_set_text(gateway_label, gateway);
        if (room_canvas_is_active() || gateway[0] == '\0') {
            lv_obj_add_flag(gateway_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(gateway_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(gateway_label);
        }
    }
    if (room_canvas_is_active()) {
        room_face_hide();
        if (state == ROOM_UI_IDLE) {
            /* A call that ended behind canvas must not leak its mood into the
             * next call. */
            room_face_reset_mood();
            if (talk_pill != NULL) {
                lv_obj_delete(talk_pill);
                talk_pill = NULL;
                talk_pill_label = NULL;
            }
        } else {
            if (talk_pill == NULL) {
                talk_pill = lv_obj_create(lv_layer_top());
                lv_obj_set_height(talk_pill, LV_SIZE_CONTENT);
                lv_obj_set_width(talk_pill, LV_SIZE_CONTENT);
                lv_obj_set_style_bg_color(talk_pill, lv_color_hex(0x181818), 0);
                lv_obj_set_style_bg_opa(talk_pill, LV_OPA_90, 0);
                lv_obj_set_style_border_width(talk_pill, 0, 0);
                lv_obj_set_style_radius(talk_pill, 12, 0);
                lv_obj_set_style_pad_hor(talk_pill, 10, 0);
                lv_obj_set_style_pad_ver(talk_pill, 6, 0);
                talk_pill_label = lv_label_create(talk_pill);
                lv_obj_set_style_text_color(talk_pill_label, lv_color_white(), 0);
                lv_obj_set_style_text_font(talk_pill_label, &lv_font_montserrat_14, 0);
                lv_obj_center(talk_pill_label);
            }
            lv_label_set_text(talk_pill_label, labels[state]);
            /* The rounded glass clips the corners; top-center stays visible. */
            lv_obj_align(talk_pill, LV_ALIGN_TOP_MID, 0, 14);
        }
        return true;
    }
    if (talk_pill != NULL) {
        lv_obj_delete(talk_pill);
        talk_pill = NULL;
        talk_pill_label = NULL;
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
    return room_ui_state_uses_face(current_state);
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
        room_board_display_brightness_set(brightness);
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
    if (!room_board_display_lock(100)) {
        return;
    }
    /* The face tick owns the hint lifetime (expiry triggers a refresh), so
     * there is no second timer to race against; re-arming just moves the
     * deadline the tick reads under the same LVGL lock. */
    room_face_show_hint(esp_timer_get_time() + (int64_t)show_ms * 1000);
    /* If face creation failed at boot the show is a no-op; keep the text
     * status instead of leaving a blank lit panel for the hold duration. */
    bool shown = room_face_is_visible();
    if (shown) {
        lv_obj_add_flag(status_label, LV_OBJ_FLAG_HIDDEN);
        if (gateway_label != NULL) {
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
    room_board_display_unlock();
    if (shown) {
        room_board_display_brightness_set(40);
    }
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
    lv_refr_now(lv_display_get_default());
    room_board_display_unlock();
    taskENTER_CRITICAL(&state_mux);
    room_ui_state_t state = current_state;
    taskEXIT_CRITICAL(&state_mux);
    int brightness = room_ui_target_brightness(state, room_canvas_is_active());
    if (brightness < ROOM_UI_CAMERA_MIN_BRIGHTNESS) {
        brightness = ROOM_UI_CAMERA_MIN_BRIGHTNESS;
    }
    esp_err_t err = room_board_display_brightness_set(brightness);
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
            lv_refr_now(lv_display_get_default());
        }
        room_board_display_unlock();
    }
    /* Repaint restores the controller-owned brightness for the live state. */
    room_ui_refresh();
}
