#include "room_ui.h"

#include "bsp/esp-bsp.h"
#include "lvgl.h"

static lv_obj_t *status_label;

void room_ui_init(void)
{
    if (bsp_display_start() == NULL || !bsp_display_lock(0)) {
        return;
    }
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
    status_label = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_color(status_label, lv_color_white(), 0);
#if LV_FONT_MONTSERRAT_28
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_28, 0);
#else
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_14, 0);
#endif
    lv_obj_center(status_label);
    lv_label_set_text(status_label, "OpenClaw");
    bsp_display_unlock();
    bsp_display_brightness_set(0);
}

void room_ui_set(room_ui_state_t state, const char *detail)
{
    static const char *labels[] = {"OpenClaw", "Listening", "Connecting", "Speaking", "Error"};
    if (status_label == NULL || !bsp_display_lock(100)) {
        return;
    }
    lv_label_set_text_fmt(
        status_label,
        detail != NULL && detail[0] != '\0' ? "%s\n%s" : "%s",
        labels[state],
        detail != NULL ? detail : "");
    bsp_display_unlock();
    /* AMOLED is fully dark while idle; active states use a deliberately low brightness. */
    bsp_display_brightness_set(state == ROOM_UI_IDLE ? 0 : 18);
}
