#include "openclaw_status_bar.h"

static void line(lv_layer_t *layer, const lv_area_t *area, lv_color_t color,
                 int x1, int y1, int x2, int y2)
{
    lv_draw_line_dsc_t draw;
    lv_draw_line_dsc_init(&draw);
    draw.p1 = (lv_point_precise_t){area->x1 + x1, area->y1 + y1};
    draw.p2 = (lv_point_precise_t){area->x1 + x2, area->y1 + y2};
    draw.color = color;
    draw.width = 2;
    draw.round_start = draw.round_end = true;
    lv_draw_line(layer, &draw);
}

static void draw_status(lv_event_t *event)
{
    openclaw_status_bar_t *bar = lv_event_get_user_data(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t area;
    lv_obj_get_coords(bar->root, &area);
    const openclaw_status_facts_t *facts = &bar->facts;
    lv_color_t active = lv_color_hex(0xbfe5d9);
    lv_color_t quiet = lv_color_hex(0x746e7c);
    lv_color_t offline = lv_color_hex(0xb8b0bd);
    unsigned strength = !facts->wifi_connected || !facts->signal_known ? 0
        : facts->rssi >= -60 ? 3 : facts->rssi >= -75 ? 2 : 1;
    for (unsigned i = 0; i < 3; ++i) {
        lv_draw_arc_dsc_t arc;
        lv_draw_arc_dsc_init(&arc);
        arc.center = (lv_point_t){area.x1 + 12, area.y1 + 18};
        arc.radius = 6 + i * 5;
        arc.width = 2;
        arc.start_angle = 225;
        arc.end_angle = 315;
        arc.rounded = true;
        arc.color = i < strength ? active : quiet;
        lv_draw_arc(layer, &arc);
    }
    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.bg_color = facts->wifi_connected ? active : quiet;
    dot.radius = LV_RADIUS_CIRCLE;
    lv_area_t dot_area = {area.x1 + 11, area.y1 + 17, area.x1 + 13, area.y1 + 19};
    lv_draw_rect(layer, &dot, &dot_area);
    if (!facts->wifi_connected) line(layer, &area, offline, 1, 2, 24, 20);

    /* Two interlocked links distinguish the Gateway from the Wi-Fi radio. */
    static const lv_point_t links[][7] = {
        {{46, 3}, {43, 3}, {39, 7}, {39, 10}, {42, 13}, {45, 13}, {49, 9}},
        {{46, 7}, {49, 7}, {53, 11}, {53, 14}, {50, 17}, {47, 17}, {43, 13}},
    };
    lv_color_t gateway = facts->gateway_connected ? active : quiet;
    for (unsigned link = 0; link < 2; ++link)
        for (unsigned i = 1; i < 7; ++i)
            line(layer, &area, gateway, links[link][i - 1].x, links[link][i - 1].y,
                 links[link][i].x, links[link][i].y);
    if (!facts->gateway_connected) line(layer, &area, offline, 38, 2, 55, 20);

    /* No battery provider exists yet: an outline with a dash means unknown. */
    lv_draw_rect_dsc_t battery;
    lv_draw_rect_dsc_init(&battery);
    battery.bg_opa = LV_OPA_TRANSP;
    battery.border_color = offline;
    battery.border_width = 1;
    battery.radius = 2;
    lv_area_t body = {area.x1 + 70, area.y1 + 5, area.x1 + 90, area.y1 + 16};
    lv_draw_rect(layer, &battery, &body);
    line(layer, &area, offline, 92, 9, 92, 12);
    line(layer, &area, offline, 77, 10, 83, 10);
}

void openclaw_status_bar_create(openclaw_status_bar_t *bar, lv_obj_t *parent)
{
    *bar = (openclaw_status_bar_t){0};
    bar->root = lv_obj_create(parent);
    lv_obj_remove_style_all(bar->root);
    lv_obj_set_size(bar->root, 94, 22);
    lv_obj_set_clickable(bar->root, false);
    lv_obj_set_scrollable(bar->root, false);
    lv_obj_add_event_cb(bar->root, draw_status, LV_EVENT_DRAW_MAIN, bar);
}

void openclaw_status_bar_set(openclaw_status_bar_t *bar, const openclaw_status_facts_t *facts)
{
    if (bar->facts.wifi_connected == facts->wifi_connected &&
        bar->facts.signal_known == facts->signal_known && bar->facts.rssi == facts->rssi &&
        bar->facts.gateway_connected == facts->gateway_connected) return;
    bar->facts = *facts;
    lv_obj_invalidate(bar->root);
}
