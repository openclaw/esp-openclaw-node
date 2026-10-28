#include "dial_input.h"
#include "dial_home.h"
#include "dial_navigation.h"
#include "openclaw_reading.h"
#include "openclaw_density.h"
#include "esp_openclaw_avatar.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static dial_input_t input;
static dial_home_t home;
static lv_indev_state_t encoder_state, pointer_state;
static int encoder_diff;
static lv_point_t pointer_point;
static lv_indev_t *pointer;
static unsigned confirmations, homes, changes;
static unsigned home_menus, home_agents, home_opens;
static dial_navigation_t navigation;
static dial_agents_snapshot_t roster;
static bool navigating;
static unsigned refreshes, previous, next, opens, reads;
static unsigned device_refreshes;
static esp_openclaw_devices_snapshot_t devices;
static dial_conversations_snapshot_t conversations;
static const char *snapshot_prefix;
static uint16_t frame[240 * 240];
static size_t requested_offset;
static int brightness = 65;
static bool brightness_failure;
static unsigned brightness_requests;
static unsigned messaging_requests, messaging_cancels;
static bool messaging_rejected;
static dial_reply_snapshot_t reply;
static unsigned reply_begins, reply_sends, reply_closes, reply_reads;
static unsigned reply_requested_preview, reply_requested_revision;
static bool reply_rejected;
static dial_creation_snapshot_t creation;
static unsigned creation_begins, creation_submits, creation_closes, creation_opens, creation_chats;
static unsigned creation_requested_revision;
static bool creation_rejected;
static uint16_t pixels[240 * 240];

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *data)
{
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x)
            frame[y * 240 + x] = ((const uint16_t *)data)[(y - area->y1) * (lv_draw_buf_width_to_stride(area->x2 - area->x1 + 1, LV_COLOR_FORMAT_RGB565) / 2) + x - area->x1];
    lv_display_flush_ready(display);
}

static void encoder_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->key = LV_KEY_ENTER;
    data->state = encoder_state;
    data->enc_diff = encoder_diff;
    encoder_diff = 0;
}

static void pointer_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->state = pointer_state;
    data->point = pointer_point;
}

static void event(dial_input_event_t kind, int value, bool pointer, void *context)
{
    (void)value;
    (void)pointer;
    (void)context;
    if (kind == DIAL_INPUT_HOME_MENU) { ++home_menus; return; }
    if (kind == DIAL_INPUT_HOME_AGENTS) { ++home_agents; return; }
    if (!navigating && !input.control && kind == DIAL_INPUT_OPEN) ++home_opens;
    if (kind == DIAL_INPUT_PREVIOUS) ++previous;
    if (kind == DIAL_INPUT_NEXT) ++next;
    if (kind == DIAL_INPUT_BACK) {
        navigating = true;
        lv_obj_set_hidden(input.arc, true);
        dial_input_set_control(&input, false);
        dial_navigation_open_local_device(&navigation);
        return;
    }
    if (navigating && kind != DIAL_INPUT_HOME) {
        if (kind == DIAL_INPUT_PREVIOUS) dial_navigation_move(&navigation, -1);
        else if (kind == DIAL_INPUT_NEXT) dial_navigation_move(&navigation, 1);
        else if (kind == DIAL_INPUT_OPEN) dial_navigation_activate(&navigation);
        return;
    }
    if (kind == DIAL_INPUT_CONFIRM) ++confirmations;
    else if (kind == DIAL_INPUT_CHANGE) ++changes;
    else {
        if (kind == DIAL_INPUT_HOME) {
            ++homes;
            navigating = false;
            if (navigation.root != NULL) lv_obj_set_hidden(navigation.root, true);
        }
        bool control = kind == DIAL_INPUT_OPEN || kind == DIAL_INPUT_PREVIOUS || kind == DIAL_INPUT_NEXT;
        lv_obj_set_flag(input.arc, LV_OBJ_FLAG_HIDDEN, !control);
        lv_obj_set_hidden(home.root, control);
        dial_input_set_control(&input, control);
    }
}

static void navigation_event(dial_navigation_event_t kind, size_t offset, void *context)
{
    (void)context;
    dial_input_set_control(&input, false);
    if (kind == DIAL_NAV_BRIGHTNESS) {
        ++brightness_requests;
        requested_offset = offset;
        if (!brightness_failure) brightness = (int)offset;
        dial_navigation_set_brightness(&navigation, brightness, brightness_failure);
    }
    else if (kind == DIAL_NAV_MESSAGING_REQUEST || kind == DIAL_NAV_MESSAGING_CANCEL) {
        if (kind == DIAL_NAV_MESSAGING_REQUEST) ++messaging_requests;
        else ++messaging_cancels;
        requested_offset = offset;
        dial_navigation_messaging_result(&navigation, !messaging_rejected);
    }
    else if (kind == DIAL_NAV_CREATION_BEGIN || kind == DIAL_NAV_CREATION_SUBMIT ||
             kind == DIAL_NAV_CREATION_CLOSE || kind == DIAL_NAV_CREATION_OPEN || kind == DIAL_NAV_CREATION_CHATS) {
        creation_requested_revision = offset;
        if (kind == DIAL_NAV_CREATION_BEGIN) {
            ++creation_begins;
            if (!creation_rejected) {
                creation = (dial_creation_snapshot_t){.state = DIAL_CREATION_REVIEW,
                    .revision = creation.revision + 1, .conversation_revision = offset,
                    .agent = conversations.agent, .can_create = navigation.messaging.state == DIAL_MESSAGING_READY};
                strcpy(creation.detail, "Creates an empty chat. No message is sent.");
            }
        } else if (kind == DIAL_NAV_CREATION_SUBMIT) {
            ++creation_submits;
            if (!creation_rejected) {
                creation.state = DIAL_CREATION_CREATING; creation.can_create = false; ++creation.revision;
                strcpy(creation.detail, "Creating an empty conversation.");
            }
        } else if (kind == DIAL_NAV_CREATION_CLOSE) {
            ++creation_closes;
            if (creation.state == DIAL_CREATION_REVIEW)
                creation = (dial_creation_snapshot_t){.revision = creation.revision + 1};
        } else if (kind == DIAL_NAV_CREATION_OPEN) {
            ++creation_opens;
            if (!creation_rejected) {
                conversations.state = conversations.preview_state = DIAL_AGENTS_READY;
                conversations.page.count = 1; conversations.selected = 0;
                conversations.preview.status = ESP_OPENCLAW_PREVIEW_EMPTY;
                ++conversations.revision; ++conversations.preview_revision;
                strcpy(conversations.page.sessions[0].title, "New conversation");
                assert(dial_navigation_show_reader(&navigation, &roster, &conversations, true));
            }
        } else {
            ++creation_chats;
            if (!creation_rejected) {
                conversations.state = DIAL_AGENTS_EMPTY; conversations.page.count = 0;
                conversations.page.has_more = false; conversations.page.offset = 0;
                ++conversations.revision;
                assert(dial_navigation_show_reader(&navigation, &roster, &conversations, false));
            }
        }
        dial_navigation_creation_update(&navigation, &creation);
        dial_navigation_creation_result(&navigation, !creation_rejected);
    }
    else if (kind == DIAL_NAV_REPLY_BEGIN || kind == DIAL_NAV_REPLY_SEND ||
             kind == DIAL_NAV_REPLY_CLOSE || kind == DIAL_NAV_REPLY_READ) {
        if (kind == DIAL_NAV_REPLY_BEGIN) {
            ++reply_begins;
            reply_requested_preview = navigation.reply_preview_revision;
            assert(offset < DIAL_REPLY_PRESET_COUNT);
            if (!reply_rejected) {
                reply = (dial_reply_snapshot_t){.state = DIAL_REPLY_PREPARING, .revision = reply.revision + 1,
                    .preview_revision = reply_requested_preview, .agent = conversations.agent};
                strcpy(reply.title, conversations.page.sessions[conversations.selected].title);
                strcpy(reply.detail, "Checking this conversation.");
                strcpy(reply.text, offset == DIAL_REPLY_CONTINUE ? "Please continue." :
                    offset == DIAL_REPLY_SUMMARIZE ? "Please summarize the conversation so far." : "Please explain the last response in more detail.");
            }
        } else if (kind == DIAL_NAV_REPLY_SEND) {
            ++reply_sends;
            reply_requested_revision = offset;
            if (!reply_rejected) {
                reply.state = DIAL_REPLY_SENDING; reply.can_send = false; ++reply.revision;
                strcpy(reply.detail, "Waiting for the Gateway.");
            }
        } else if (kind == DIAL_NAV_REPLY_CLOSE) {
            ++reply_closes;
            reply_requested_revision = offset;
            if (reply.state == DIAL_REPLY_PREPARING || reply.state == DIAL_REPLY_REVIEW)
                reply = (dial_reply_snapshot_t){.revision = reply.revision + 1};
        } else {
            ++reply_reads;
            reply_requested_revision = offset;
            if (!reply_rejected) {
                ++conversations.preview_revision;
                assert(dial_navigation_show_reader(&navigation, &roster, &conversations, true));
            }
        }
        dial_navigation_reply_update(&navigation, &reply);
        dial_navigation_reply_result(&navigation, !reply_rejected);
    }
    else if (kind == DIAL_NAV_CONVERSATIONS) { ++opens; requested_offset = offset; }
    else if (kind == DIAL_NAV_READ) { ++reads; requested_offset = offset; }
    else if (kind == DIAL_NAV_RESUME) {
        if (!dial_navigation_show_reader(&navigation, &roster, &conversations, false))
            dial_navigation_open(&navigation, true);
    }
    else if (kind == DIAL_NAV_DEVICES_REFRESH) ++device_refreshes;
    else if (kind == DIAL_NAV_REFRESH || kind == DIAL_NAV_SESSIONS_REFRESH) { ++refreshes; requested_offset = offset; }
    else if (kind == DIAL_NAV_HOME || kind == DIAL_NAV_CONTROL) {
        navigating = false;
        lv_obj_set_hidden(navigation.root, true);
        lv_obj_set_hidden(home.root, kind != DIAL_NAV_HOME);
        lv_obj_set_hidden(input.arc, kind != DIAL_NAV_CONTROL);
        dial_input_set_control(&input, kind == DIAL_NAV_CONTROL);
    }
}

static void advance(unsigned milliseconds)
{
    for (unsigned i = 0; i < milliseconds; i += 40) {
        lv_tick_inc(40);
        lv_timer_handler();
    }
}

static void control(void)
{
    lv_obj_set_hidden(home.root, true);
    lv_obj_set_hidden(input.arc, false);
    dial_input_set_control(&input, true);
    lv_arc_set_value(input.arc, 50);
    advance(40);
}

static void check_home_bounds(lv_obj_t *object)
{
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    const int x[] = {area.x1 - 120, area.x2 - 120};
    const int y[] = {area.y1 - 120, area.y2 - 120};
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j)
            assert(x[i] * x[i] + y[j] * y[j] <= 112 * 112);
    /* Scroll content is clipped by this bounded viewport, not by its label. */
    if (object == navigation.preview_body) return;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i)
        check_home_bounds(lv_obj_get_child(object, i));
}

static void touch(int x, int y, bool pressed)
{
    pointer_point = (lv_point_t){x, y};
    pointer_state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    lv_indev_read(pointer);
    advance(40);
}

static void snapshot(const char *name)
{
    if (snapshot_prefix == NULL) return;
    char path[1024];
    assert(snprintf(path, sizeof(path), "%s-%s.ppm", snapshot_prefix, name) < (int)sizeof(path));
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    fprintf(file, "P6\n240 240\n255\n");
    for (unsigned i = 0; i < 240 * 240; ++i) {
        unsigned pixel = frame[i];
        unsigned char rgb[] = {(pixel >> 11) * 255 / 31, ((pixel >> 5) & 63) * 255 / 63, (pixel & 31) * 255 / 31};
        assert(fwrite(rgb, 1, 3, file) == 3);
    }
    assert(fclose(file) == 0);
}

static unsigned reader_boundary_failures;

static void check_reader_limits(lv_indev_t *encoder, const char *name)
{
    lv_obj_t *body = navigation.preview_body;
    lv_obj_update_layout(body);
    int32_t saved_y = lv_obj_get_scroll_y(body);
    dial_navigation_view_t page = navigation.view;
    unsigned before = confirmations;
    for (int direction = 1; direction >= -1; direction -= 2) {
        lv_obj_scroll_to_y(body, 0, LV_ANIM_OFF);
        encoder_diff = 100 * direction;
        lv_indev_read(encoder);
        if (lv_obj_get_scroll_top(body) < 0 || lv_obj_get_scroll_bottom(body) < 0) {
            fprintf(stderr, "%s: encoder escaped the %s boundary\n", name, direction > 0 ? "bottom" : "top");
            ++reader_boundary_failures;
        }
        assert(navigation.view == page && confirmations == before);
        if (direction > 0 && snapshot_prefix != NULL) snapshot(name);
    }
    lv_obj_scroll_to_y(body, saved_y, LV_ANIM_OFF);
    advance(40);
}

static unsigned active_pixels(int x1, int x2)
{
    unsigned count = 0;
    const unsigned active = (0xbf >> 3) << 11 | (0xe5 >> 2) << 5 | (0xd9 >> 3);
    for (int y = 20; y < 42; ++y)
        for (int x = x1; x < x2; ++x)
            if (frame[y * 240 + x] == active) ++count;
    return count;
}

static unsigned paint_status(openclaw_status_facts_t facts, const char *name)
{
    openclaw_status_bar_set(&home.status, &facts);
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
    snapshot(name);
    return active_pixels(73, 100);
}

static void check_status_bar(void)
{
    openclaw_status_facts_t status = {0};
    assert(paint_status(status, "status-offline") == 0);
    uint16_t battery[22 * 26];
    for (int y = 0; y < 22; ++y)
        memcpy(&battery[y * 26], &frame[(20 + y) * 240 + 141], 26 * sizeof(uint16_t));
    status.wifi_connected = true;
    unsigned unknown = paint_status(status, "status-unknown");
    assert(unknown > 0);
    status.signal_known = true;
    status.rssi = -76;
    unsigned weak = paint_status(status, "status-weak");
    status.rssi = -75;
    unsigned medium = paint_status(status, "status-medium");
    status.rssi = -61;
    assert(paint_status(status, "status-medium-edge") == medium);
    status.rssi = -60;
    unsigned strong = paint_status(status, "status-strong");
    assert(strong > medium && medium > weak && weak > unknown);
    assert(active_pixels(109, 132) == 0);
    status.gateway_connected = true;
    status.rssi = 0;
    assert(paint_status(status, "status-online") == strong);
    assert(active_pixels(109, 132) > 0);
    for (int y = 0; y < 22; ++y)
        assert(memcmp(&battery[y * 26], &frame[(20 + y) * 240 + 141], 26 * sizeof(uint16_t)) == 0);
    status.signal_known = false;
    assert(paint_status(status, "status-unavailable-again") == unknown);
    puts("Status strip: measured bands, unavailable signal, independent link, unknown battery passed");
}

static uint32_t avatar_hash(int x, int y, int size)
{
    lv_refr_now(NULL);
    uint32_t hash = 2166136261u;
    for (int row = y; row < y + size; ++row)
        for (int col = x; col < x + size; ++col)
            hash = (hash ^ frame[row * 240 + col]) * 16777619u;
    return hash;
}

static void check_avatar_identity(void)
{
    const struct { const char *emoji, *name, *id; esp_openclaw_avatar_kind_t kind; const char *text; } cases[] = {
        {"🦊", "Assistant", "a", ESP_OPENCLAW_AVATAR_FOX, ""},
        {"🦊️", "Assistant", "a", ESP_OPENCLAW_AVATAR_FOX, ""},
        {"🦞", "Kitsune", "a", ESP_OPENCLAW_AVATAR_LOBSTER, ""},
        {"🦞️", "Assistant", "a", ESP_OPENCLAW_AVATAR_LOBSTER, ""},
        {"🦊x", "two words", "a", ESP_OPENCLAW_AVATAR_TEXT, "TW"},
        {"🔮", "two-words", "a", ESP_OPENCLAW_AVATAR_TEXT, "TW"},
        {"", "Kitsune", "a", ESP_OPENCLAW_AVATAR_TEXT, "K"},
        {"!", "Assistant", "a", ESP_OPENCLAW_AVATAR_TEXT, "!"},
        {" ", "two_words", "a", ESP_OPENCLAW_AVATAR_TEXT, "TW"},
        {"", "狐", "fox", ESP_OPENCLAW_AVATAR_TEXT, "F"},
        {"", "", "", ESP_OPENCLAW_AVATAR_TEXT, "?"},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        esp_openclaw_agent_t agent = {0};
        strcpy(agent.emoji, cases[i].emoji);
        strcpy(agent.name, cases[i].name);
        strcpy(agent.id, cases[i].id);
        esp_openclaw_avatar_t avatar = esp_openclaw_agent_avatar(&agent);
        assert(avatar.kind == cases[i].kind && strcmp(avatar.text, cases[i].text) == 0);
    }
}

static void press_encoder(lv_indev_t *encoder)
{
    encoder_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(encoder); advance(40);
    encoder_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(encoder); advance(40);
}

static void messaging_snapshot(const char *name)
{
    lv_obj_update_layout(navigation.root);
    for (unsigned child = 0; child < lv_obj_get_child_count(navigation.root); ++child)
        check_home_bounds(lv_obj_get_child(navigation.root, child));
    lv_obj_t *labels[] = {navigation.messaging_status, navigation.messaging_detail, navigation.names[0]};
    for (unsigned i = 0; i < sizeof(labels) / sizeof(*labels); ++i) {
        lv_point_t size;
        lv_text_get_size(&size, lv_label_get_text(labels[i]), lv_obj_get_style_text_font(labels[i], 0), 0, 0,
            i == 1 ? lv_obj_get_width(labels[i]) : LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (size.x > lv_obj_get_width(labels[i]) || size.y > lv_obj_get_height(labels[i]))
            fprintf(stderr, "%s: label %s needs %ld x %ld, has %ld x %ld\n", name,
                lv_label_get_text(labels[i]), (long)size.x, (long)size.y,
                (long)lv_obj_get_width(labels[i]), (long)lv_obj_get_height(labels[i]));
        assert(size.x <= lv_obj_get_width(labels[i]));
        assert(size.y <= lv_obj_get_height(labels[i]));
    }
    snapshot(name);
}

static void check_messaging(lv_indev_t *encoder)
{
    unsigned before = confirmations;
    navigating = true;
    lv_obj_set_hidden(home.root, true);
    lv_obj_set_hidden(input.arc, true);
    dial_input_set_control(&input, false);
    dial_navigation_open_settings(&navigation, DIAL_NAV_SETTINGS);
    encoder_diff = 2;
    lv_indev_read(encoder); advance(40);
    assert(strcmp(lv_label_get_text(navigation.names[2]), "Messaging") == 0);
    press_encoder(encoder);
    assert(navigation.view == DIAL_NAV_MESSAGING && messaging_requests == 0);
    assert(strcmp(lv_label_get_text(navigation.messaging_status), "Access off") == 0);
    assert(strstr(lv_label_get_text(navigation.messaging_detail), "Read only") != NULL);
    messaging_snapshot("messaging-off");

    dial_messaging_snapshot_t permission = {.revision = 41};
    dial_navigation_messaging_update(&navigation, &permission);
    touch(120, 184, true); touch(120, 184, false);
    assert(messaging_requests == 1 && requested_offset == 41 && messaging_cancels == 0);
    assert(navigation.messaging.state == DIAL_MESSAGING_OFF); // Intent is not permission.
    touch(120, 184, true);
    permission.state = DIAL_MESSAGING_REQUESTING;
    ++permission.revision;
    dial_navigation_messaging_update(&navigation, &permission);
    touch(120, 184, false);
    assert(messaging_requests == 1 && messaging_cancels == 0);
    assert(strcmp(lv_label_get_text(navigation.messaging_status), "Requesting") == 0);
    messaging_snapshot("messaging-requesting");
    permission.state = DIAL_MESSAGING_WAITING;
    ++permission.revision;
    strcpy(permission.request_id, "approval-1");
    dial_navigation_messaging_update(&navigation, &permission);
    assert(strcmp(lv_label_get_text(navigation.names[0]), "Cancel") == 0);
    assert(strstr(lv_label_get_text(navigation.messaging_detail), "Approve on Gateway") != NULL);
    messaging_snapshot("messaging-waiting");
    touch(68, 44, true); touch(68, 44, false);
    assert(navigation.view == DIAL_NAV_SETTINGS && navigation.focus == 2 && messaging_cancels == 0);
    press_encoder(encoder);
    assert(navigation.view == DIAL_NAV_MESSAGING && navigation.messaging.state == DIAL_MESSAGING_WAITING);

    // A new permission observation cancels a held pointer or encoder action.
    touch(120, 184, true);
    permission.state = DIAL_MESSAGING_CONNECTING;
    ++permission.revision;
    dial_navigation_messaging_update(&navigation, &permission);
    touch(120, 184, false);
    assert(messaging_cancels == 0);
    assert(strcmp(lv_label_get_text(navigation.messaging_status), "Reconnecting") == 0);
    messaging_snapshot("messaging-connecting");
    encoder_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(encoder); advance(40);
    ++permission.revision;
    dial_navigation_messaging_update(&navigation, &permission);
    encoder_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(encoder); advance(40);
    assert(messaging_cancels == 0);
    press_encoder(encoder);
    assert(messaging_cancels == 1 && requested_offset == permission.revision);
    permission.state = DIAL_MESSAGING_READY;
    ++permission.revision;
    dial_navigation_messaging_update(&navigation, &permission);
    assert(strcmp(lv_label_get_text(navigation.messaging_status), "Access ready") == 0);
    assert(strcmp(lv_label_get_text(navigation.names[0]), "Turn off") == 0);
    messaging_snapshot("messaging-ready");
    press_encoder(encoder);
    assert(navigation.view == DIAL_NAV_MESSAGING && messaging_requests == 1 && messaging_cancels == 2);
    assert(requested_offset == permission.revision);
    assert(navigation.messaging.state == DIAL_MESSAGING_READY); // Wait for the owner to confirm local off.
    permission.state = DIAL_MESSAGING_OFF;
    ++permission.revision;
    dial_navigation_messaging_update(&navigation, &permission);
    touch(68, 44, true); touch(68, 44, false);
    assert(navigation.view == DIAL_NAV_SETTINGS && navigation.focus == 2);

    const dial_messaging_state_t retry_states[] = {
        DIAL_MESSAGING_REJECTED, DIAL_MESSAGING_EXPIRED, DIAL_MESSAGING_ERROR,
    };
    const char *snapshots[] = {"messaging-rejected", "messaging-expired", "messaging-error"};
    for (unsigned i = 0; i < sizeof(retry_states) / sizeof(*retry_states); ++i) {
        permission.state = retry_states[i];
        ++permission.revision;
        dial_navigation_messaging_update(&navigation, &permission);
        dial_navigation_open_settings(&navigation, DIAL_NAV_MESSAGING);
        assert(strcmp(lv_label_get_text(navigation.names[0]), "Request") == 0);
        messaging_snapshot(snapshots[i]);
        press_encoder(encoder);
        assert(messaging_requests == i + 2 && requested_offset == permission.revision);
        assert(navigation.messaging.state == retry_states[i]);
    }
    messaging_rejected = true;
    press_encoder(encoder);
    assert(strstr(lv_label_get_text(navigation.messaging_detail), "Connect to Gateway") != NULL);
    dial_navigation_messaging_update(&navigation, &permission);
    assert(strstr(lv_label_get_text(navigation.messaging_detail), "Connect to Gateway") != NULL);
    messaging_snapshot("messaging-offline-request");
    messaging_rejected = false;
    // The same state with a new request identity also invalidates a held action.
    touch(120, 184, true);
    strcpy(permission.request_id, "approval-2");
    dial_navigation_messaging_update(&navigation, &permission);
    unsigned requests = messaging_requests;
    touch(120, 184, false);
    assert(messaging_requests == requests);
    touch(120, 184, true); touch(150, 184, true); touch(150, 184, false);
    assert(messaging_requests == requests);
    touch(120, 184, true); advance(1000); touch(120, 184, false);
    assert(!navigating && messaging_requests == requests && confirmations == before);
    puts("Messaging: permission-only state, fenced request/cancel, retry, Back and circular layout passed");
}

static void check_replies(lv_indev_t *encoder)
{
    unsigned confirms = confirmations;
    navigating = true;
    lv_obj_set_hidden(home.root, true);
    lv_obj_set_hidden(input.arc, true);
    dial_input_set_control(&input, false);
    roster.state = DIAL_AGENTS_READY;
    ++roster.revision;
    roster.page.count = roster.page.total = 1;
    strcpy(roster.page.agents[0].id, "beta");
    strcpy(roster.page.agents[0].name, "Beta");
    conversations = (dial_conversations_snapshot_t){.state = DIAL_AGENTS_READY,
        .preview_state = DIAL_AGENTS_READY, .revision = 50, .preview_revision = 51,
        .agent = roster.page.agents[0], .page = {.count = 1},
        .preview = {.status = ESP_OPENCLAW_PREVIEW_EMPTY}};
    strcpy(conversations.page.sessions[0].title, "A careful next step");
    assert(dial_navigation_show_reader(&navigation, &roster, &conversations, true));
    lv_obj_set_hidden(navigation.root, false); // The application owns view visibility.
    assert(lv_obj_is_visible(navigation.root));
    assert(strcmp(lv_label_get_text(navigation.names[0]), "Reply") == 0);
    snapshot("reply-empty-preview");
    press_encoder(encoder);
    assert(navigation.view == DIAL_NAV_REPLY_PICKER && reply_begins == 0);
    assert(strstr(lv_label_get_text(navigation.description), "Messaging access needed") != NULL);
    snapshot("reply-access-needed");
    press_encoder(encoder);
    assert(navigation.view == DIAL_NAV_MESSAGING);
    dial_messaging_snapshot_t access = {.state = DIAL_MESSAGING_READY, .revision = 100};
    dial_navigation_messaging_update(&navigation, &access);
    touch(68, 44, true); touch(68, 44, false);
    assert(navigation.view == DIAL_NAV_REPLY_PICKER && reply_begins == 0);
    snapshot("reply-presets");
    encoder_diff = 1; lv_indev_read(encoder); advance(40);
    press_encoder(encoder);
    assert(reply_begins == 1 && reply_requested_preview == 51 && reply_sends == 0);
    assert(navigation.view == DIAL_NAV_REPLY_REVIEW);
    assert(strcmp(lv_label_get_text(navigation.heading), "Preparing") == 0);
    snapshot("reply-preparing");
    reply.state = DIAL_REPLY_REVIEW;
    strcpy(reply.detail, "Review before sending.");
    reply.can_send = true;
    ++reply.revision;
    dial_navigation_reply_update(&navigation, &reply);
    const char *review = lv_label_get_text(navigation.preview_text);
    assert(strstr(review, "Beta\nA careful next step") == review);
    const char *message = strstr(review, reply.text);
    const char *status = strstr(review, reply.detail);
    const char *identity = strstr(review, "Agent: beta");
    assert(message != NULL && status != NULL && identity != NULL);
    assert(message < status && status < identity);
    check_reader_limits(encoder, "reply-review-bottom");
    snapshot("reply-review");
    press_encoder(encoder);
    assert(navigation.view == DIAL_NAV_REPLY_ACTIONS && reply_sends == 0);
    assert(strcmp(lv_label_get_text(navigation.names[0]), "Send") == 0);
    snapshot("reply-actions");
    touch(120, 82, true);
    ++reply.revision; // A changed owner revision consumes the held Send.
    dial_navigation_reply_update(&navigation, &reply);
    touch(120, 82, false);
    assert(reply_sends == 0);
    encoder_state = LV_INDEV_STATE_PRESSED; lv_indev_read(encoder); advance(40);
    ++reply.revision;
    dial_navigation_reply_update(&navigation, &reply);
    encoder_state = LV_INDEV_STATE_RELEASED; lv_indev_read(encoder); advance(40);
    assert(reply_sends == 0);
    access.state = DIAL_MESSAGING_OFF; ++access.revision;
    touch(120, 82, true);
    dial_navigation_messaging_update(&navigation, &access);
    touch(120, 82, false);
    assert(reply_sends == 0 && strcmp(lv_label_get_text(navigation.names[0]), "Messaging") == 0);
    access.state = DIAL_MESSAGING_READY; ++access.revision;
    dial_navigation_messaging_update(&navigation, &access);
    unsigned sent_revision = reply.revision;
    press_encoder(encoder);
    assert(reply_sends == 1 && reply_requested_revision == sent_revision);
    assert(strcmp(lv_label_get_text(navigation.heading), "Sending") == 0);
    snapshot("reply-sending");
    touch(68, 44, true); touch(68, 44, false);
    assert(navigation.view == DIAL_NAV_PREVIEW && reply.state == DIAL_REPLY_SENDING);
    ++conversations.preview_revision;
    dial_navigation_conversations_update(&navigation, &conversations);
    press_encoder(encoder);
    assert(navigation.view == DIAL_NAV_REPLY_REVIEW && reply_begins == 1);
    reply.state = DIAL_REPLY_ACCEPTED; ++reply.revision; reply.can_read = true;
    strcpy(reply.detail, "Accepted. Waiting for the agent.");
    dial_navigation_reply_update(&navigation, &reply);
    snapshot("reply-accepted");
    press_encoder(encoder);
    assert(strcmp(lv_label_get_text(navigation.names[0]), "Read reply") == 0 && navigation.count == 2);
    reply.state = DIAL_REPLY_UNKNOWN; ++reply.revision;
    strcpy(reply.detail, "Outcome unknown. Check the conversation before retrying.");
    dial_navigation_reply_update(&navigation, &reply);
    touch(68, 44, true); touch(68, 44, false);
    assert(strstr(lv_label_get_text(navigation.preview_text), "Check this conversation before trying again.") != NULL);
    snapshot("reply-unknown");
    reply.state = DIAL_REPLY_SENT; ++reply.revision;
    strcpy(reply.detail, "Sent. Check the conversation; agent work may continue.");
    dial_navigation_reply_update(&navigation, &reply);
    assert(strcmp(lv_label_get_text(navigation.heading), "Sent") == 0);
    assert(strstr(lv_label_get_text(navigation.preview_text), "agent work may continue.") != NULL);
    snapshot("reply-sent");
    reply.state = DIAL_REPLY_FAILED; ++reply.revision;
    strcpy(reply.detail, "The Gateway rejected this message.");
    dial_navigation_reply_update(&navigation, &reply);
    assert(strcmp(lv_label_get_text(navigation.heading), "Reply failed") == 0);
    assert(strstr(lv_label_get_text(navigation.preview_text), reply.detail) != NULL);
    snapshot("reply-failed");
    press_encoder(encoder);
    assert(strcmp(lv_label_get_text(navigation.names[1]), "New reply") == 0);
    press_encoder(encoder);
    assert(reply_reads == 1 && navigation.view == DIAL_NAV_PREVIEW);
    press_encoder(encoder);
    assert(navigation.view == DIAL_NAV_REPLY_PICKER && reply_begins == 1); // Refreshed preview is a new context.
    reply_rejected = true;
    press_encoder(encoder);
    assert(reply_begins == 2 && reply_sends == 1);
    assert(strcmp(lv_label_get_text(navigation.hint), "Back to retry") == 0);
    reply_rejected = false;
    encoder_diff = 2; lv_indev_read(encoder); advance(40);
    press_encoder(encoder);
    assert(reply_begins == 3 && strstr(reply.text, "explain") != NULL);
    reply.state = DIAL_REPLY_REVIEW; reply.can_send = true; ++reply.revision;
    strcpy(reply.agent.name, "A thoughtful research assistant with a long display name");
    strcpy(reply.agent.id, "research-and-planning-assistant-with-a-long-agent-identifier");
    strcpy(reply.title, "Reviewing the next engineering step and the practical tradeoffs in a long conversation");
    strcpy(reply.detail, "Review before sending.");
    dial_navigation_reply_update(&navigation, &reply);
    assert(strstr(lv_label_get_text(navigation.preview_text), reply.agent.name) != NULL);
    assert(strstr(lv_label_get_text(navigation.preview_text), reply.agent.id) != NULL);
    assert(strstr(lv_label_get_text(navigation.preview_text), reply.title) != NULL);
    assert(strstr(lv_label_get_text(navigation.preview_text), reply.text) != NULL);
    check_reader_limits(encoder, "reply-long-review-bottom");
    unsigned closes = reply_closes;
    touch(68, 44, true); touch(68, 44, false);
    assert(reply_closes == closes + 1 && reply.state == DIAL_REPLY_IDLE && navigation.view == DIAL_NAV_REPLY_PICKER);
    assert(reply_sends == 1 && confirmations == confirms);
    for (unsigned child = 0; child < lv_obj_get_child_count(navigation.root); ++child)
        check_home_bounds(lv_obj_get_child(navigation.root, child));
    puts("Quick replies: empty preview, permission return, exact review, fenced Send, retained outcome and refreshed reader passed");
}

static void check_creation(lv_indev_t *encoder)
{
    unsigned confirms = confirmations;
    navigating = true;
    lv_obj_set_hidden(home.root, true);
    lv_obj_set_hidden(input.arc, true);
    lv_obj_set_hidden(navigation.root, false);
    dial_input_set_control(&input, false);
    roster = (dial_agents_snapshot_t){.state = DIAL_AGENTS_READY, .revision = roster.revision + 1,
        .page = {.count = 1, .total = 1}};
    strcpy(roster.page.agents[0].id, "beta");
    strcpy(roster.page.agents[0].name, "Beta");
    conversations = (dial_conversations_snapshot_t){.state = DIAL_AGENTS_EMPTY, .revision = 200,
        .agent = roster.page.agents[0]};
    assert(dial_navigation_show_reader(&navigation, &roster, &conversations, false));
    dial_messaging_snapshot_t access = {.state = DIAL_MESSAGING_OFF, .revision = 300};
    dial_navigation_messaging_update(&navigation, &access);
    assert(strcmp(lv_label_get_text(navigation.names[0]), "New chat") == 0 && navigation.count == 3);
    snapshot("creation-empty-list");
    encoder_diff = 2; lv_indev_read(encoder); advance(40);
    assert(strcmp(lv_label_get_text(navigation.names[0]), "Back") == 0);
    encoder_diff = -2; lv_indev_read(encoder); advance(40);
    assert(strcmp(lv_label_get_text(navigation.names[0]), "New chat") == 0);
    touch(120, 164, true);
    ++conversations.revision;
    dial_navigation_conversations_update(&navigation, &conversations);
    touch(120, 164, false);
    assert(creation_begins == 0 && navigation.view == DIAL_NAV_SESSIONS);
    press_encoder(encoder);
    assert(creation_begins == 1 && creation_requested_revision == conversations.revision);
    assert(creation_submits == 0 && navigation.view == DIAL_NAV_CREATION);
    assert(strcmp(lv_label_get_text(navigation.names[0]), "Messaging") == 0);
    assert(strstr(lv_label_get_text(navigation.preview_text), "Creates an empty chat") != NULL);
    assert(strstr(lv_label_get_text(navigation.preview_text), "Agent: beta") != NULL);
    snapshot("creation-access-needed");
    press_encoder(encoder);
    assert(navigation.view == DIAL_NAV_MESSAGING);
    access.state = DIAL_MESSAGING_READY; ++access.revision;
    dial_navigation_messaging_update(&navigation, &access);
    touch(68, 44, true); touch(68, 44, false);
    assert(navigation.view == DIAL_NAV_CREATION && creation_begins == 1);
    assert(strcmp(lv_label_get_text(navigation.names[0]), "Chats") == 0);
    snapshot("creation-stale-agent");
    creation.can_create = true; ++creation.revision;
    dial_navigation_creation_update(&navigation, &creation);
    assert(strcmp(lv_label_get_text(navigation.names[0]), "Create") == 0);
    snapshot("creation-review");
    check_reader_limits(encoder, "creation-review-bottom");
    touch(120, 190, true);
    creation.can_create = false; ++creation.revision;
    dial_navigation_creation_update(&navigation, &creation);
    touch(120, 190, false);
    assert(creation_submits == 0);
    creation.can_create = true; ++creation.revision;
    dial_navigation_creation_update(&navigation, &creation);
    encoder_state = LV_INDEV_STATE_PRESSED; lv_indev_read(encoder); advance(40);
    ++creation.revision;
    dial_navigation_creation_update(&navigation, &creation);
    encoder_state = LV_INDEV_STATE_RELEASED; lv_indev_read(encoder); advance(40);
    assert(creation_submits == 0);
    unsigned submitted_revision = creation.revision;
    press_encoder(encoder);
    assert(creation_submits == 1 && creation_requested_revision == submitted_revision);
    assert(strcmp(lv_label_get_text(navigation.heading), "Creating") == 0);
    snapshot("creation-pending");
    press_encoder(encoder);
    assert(creation_submits == 1);
    touch(68, 44, true); touch(68, 44, false);
    assert(navigation.view == DIAL_NAV_SESSIONS && creation.state == DIAL_CREATION_CREATING);
    ++conversations.revision;
    dial_navigation_conversations_update(&navigation, &conversations);
    press_encoder(encoder);
    assert(navigation.view == DIAL_NAV_CREATION && creation_begins == 1);
    creation.state = DIAL_CREATION_CREATED; creation.can_open = true; ++creation.revision;
    strcpy(creation.detail, "Created. Open it to choose a reply.");
    dial_navigation_creation_update(&navigation, &creation);
    assert(strcmp(lv_label_get_text(navigation.names[0]), "Open") == 0);
    snapshot("creation-created");
    touch(120, 190, true);
    creation.can_open = false; ++creation.revision;
    dial_navigation_creation_update(&navigation, &creation);
    touch(120, 190, false);
    assert(creation_opens == 0);
    creation.can_open = true; ++creation.revision;
    dial_navigation_creation_update(&navigation, &creation);
    press_encoder(encoder);
    assert(creation_opens == 1 && navigation.view == DIAL_NAV_PREVIEW);
    assert(strcmp(lv_label_get_text(navigation.preview_text), "No messages yet") == 0);
    assert(strcmp(lv_label_get_text(navigation.names[0]), "Reply") == 0);
    snapshot("creation-opened");
    touch(68, 44, true); touch(68, 44, false);
    encoder_diff = 1; lv_indev_read(encoder); advance(40);
    press_encoder(encoder);
    assert(creation_begins == 2 && creation_submits == 1);
    creation_rejected = true;
    press_encoder(encoder);
    assert(creation_submits == 2 && navigation.creation_action_failed);
    assert(strstr(lv_label_get_text(navigation.preview_text), "Action unavailable") != NULL);
    creation_rejected = false;
    creation.state = DIAL_CREATION_FAILED; creation.can_create = false; ++creation.revision;
    strcpy(creation.detail, "Could not queue. Review and try again.");
    dial_navigation_creation_update(&navigation, &creation);
    assert(strcmp(lv_label_get_text(navigation.names[0]), "Retry") == 0);
    snapshot("creation-failed");
    press_encoder(encoder);
    assert(creation_begins == 3 && creation_submits == 2 && creation.state == DIAL_CREATION_REVIEW);
    press_encoder(encoder);
    assert(creation_submits == 3);
    creation.state = DIAL_CREATION_UNKNOWN; ++creation.revision;
    strcpy(creation.detail, "Outcome unknown. Check Chats before creating another.");
    dial_navigation_creation_update(&navigation, &creation);
    assert(strcmp(lv_label_get_text(navigation.names[0]), "Chats") == 0);
    snapshot("creation-unknown");
    press_encoder(encoder);
    assert(creation_chats == 1 && navigation.view == DIAL_NAV_SESSIONS);
    press_encoder(encoder);
    assert(creation_begins == 4 && creation_submits == 3);
    strcpy(creation.agent.name, "A thoughtful research assistant with a long display name");
    strcpy(creation.agent.id, "research-and-planning-assistant-with-a-long-agent-identifier");
    ++creation.revision;
    dial_navigation_creation_update(&navigation, &creation);
    check_reader_limits(encoder, "creation-long-agent-bottom");
    unsigned closes = creation_closes;
    touch(68, 44, true); touch(68, 44, false);
    assert(creation_closes == closes + 1 && creation.state == DIAL_CREATION_IDLE);
    assert(navigation.view == DIAL_NAV_SESSIONS && confirmations == confirms && reply_sends == 1);
    conversations.state = DIAL_AGENTS_READY; conversations.page.count = 2;
    conversations.page.has_more = true; conversations.page.next_offset = 17;
    ++conversations.revision;
    dial_navigation_conversations_update(&navigation, &conversations);
    encoder_diff = 2; lv_indev_read(encoder); advance(40);
    assert(strcmp(lv_label_get_text(navigation.names[2]), "New chat") == 0);
    encoder_diff = 1; lv_indev_read(encoder); advance(40);
    assert(strcmp(lv_label_get_text(navigation.names[0]), "Next page") == 0);
    unsigned refresh = refreshes;
    press_encoder(encoder);
    assert(refreshes == refresh + 1 && requested_offset == 17 && creation_begins == 4);
    creation = (dial_creation_snapshot_t){.state = DIAL_CREATION_CREATED,
        .revision = creation.revision + 1, .conversation_revision = conversations.revision - 1};
    strcpy(creation.agent.name, "Previous agent");
    strcpy(creation.agent.id, "previous-agent");
    dial_navigation_creation_update(&navigation, &creation);
    creation_rejected = true;
    encoder_diff = -1; lv_indev_read(encoder); advance(40);
    press_encoder(encoder);
    assert(creation_begins == 5 && navigation.view == DIAL_NAV_SESSIONS);
    assert(strcmp(lv_label_get_text(navigation.hint), "Try Refresh") == 0);
    assert(strcmp(lv_label_get_text(navigation.heading), "Beta") == 0);
    snapshot("creation-prepare-failed");
    creation.agent = conversations.agent;
    creation.conversation_revision = conversations.revision;
    creation.can_open = true; ++creation.revision;
    dial_navigation_creation_update(&navigation, &creation);
    press_encoder(encoder);
    assert(navigation.view == DIAL_NAV_CREATION);
    press_encoder(encoder); // A failed Open must still allow the stored status to reopen.
    assert(navigation.creation_action_failed);
    touch(68, 44, true); touch(68, 44, false);
    press_encoder(encoder);
    assert(navigation.view == DIAL_NAV_CREATION && creation_begins == 5);
    creation_rejected = false;
    for (unsigned child = 0; child < lv_obj_get_child_count(navigation.root); ++child)
        check_home_bounds(lv_obj_get_child(navigation.root, child));
    puts("New conversation: empty list, reviewed creation, permission recovery, stale presses, outcomes and safe retry passed");
}

int main(int argc, char **argv)
{
    check_avatar_identity();
    snapshot_prefix = argc > 1 ? argv[1] : NULL;
    lv_init();
    lv_display_t *display = lv_display_create(240, 240);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x15101b), 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_hex(0x362031), 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);
    lv_obj_set_scrollable(screen, false);
    lv_obj_set_clickable(screen, true);
    dial_home_create(&home, screen);
    check_status_bar();
    const char *moods[] = {"neutral", "happy", "excited", "thinking", "sleepy", "sad"};
    const uint32_t phases[] = {0, 200, 1900, 3700};
    for (unsigned mood = 0; mood < sizeof(moods) / sizeof(moods[0]); ++mood) {
        for (unsigned phase = 0; phase < sizeof(phases) / sizeof(phases[0]); ++phase) {
            dial_home_animate(&home, moods[mood], phases[phase]);
            lv_obj_update_layout(screen);
            for (unsigned i = 0; i < lv_obj_get_child_count(home.root); ++i)
                check_home_bounds(lv_obj_get_child(home.root, i));
        }
        char name[32];
        snprintf(name, sizeof(name), "home-mood-%s", moods[mood]);
        snapshot(name);
    }
    lv_obj_set_hidden(home.root, true);
    assert(!lv_obj_is_visible(home.eyes[0]) && !lv_obj_is_visible(home.eyes[1]) && !lv_obj_is_visible(home.mouth));
    lv_obj_set_hidden(home.root, false);
    assert(lv_obj_is_visible(home.eyes[0]) && lv_obj_is_visible(home.eyes[1]) && lv_obj_is_visible(home.mouth));
    dial_home_animate(&home, "neutral", 200);
    dial_home_context_t home_context = {0};
    const openclaw_status_facts_t online_status = {.wifi_connected = true, .gateway_connected = true};
    const openclaw_status_facts_t gateway_offline = {.wifi_connected = true};
    dial_home_render(&home, &online_status, true, NULL, &home_context);
    lv_obj_update_layout(screen);
    for (unsigned i = 0; i < lv_obj_get_child_count(home.root); ++i)
        check_home_bounds(lv_obj_get_child(home.root, i));
    assert(strcmp(lv_label_get_text(home.action), "Choose agent") == 0);
    dial_home_render(&home, &gateway_offline, false, NULL, &home_context);
    assert(strcmp(lv_label_get_text(home.headline), "Gateway offline") == 0);
    dial_home_render(&home, &online_status, false, NULL, &home_context);
    assert(strcmp(lv_label_get_text(home.headline), "Ready when you are") == 0);
    assert(strcmp(lv_label_get_text(home.action), "Choose agent") == 0);
    char activity[] = "Action failed";
    dial_home_render(&home, &online_status, true, activity, &home_context);
    memset(activity, 'x', strlen(activity));
    assert(strcmp(lv_label_get_text(home.headline), "Action failed") == 0);
    lv_obj_update_layout(screen);
    check_home_bounds(home.headline);
    home_context = (dial_home_context_t){.available = true};
    strcpy(home_context.agent_name, "A very long assistant name that needs truncation");
    dial_home_render(&home, &online_status, false, NULL, &home_context);
    assert(home.destination == DIAL_HOME_READER);
    assert(strcmp(lv_label_get_text(home.title), home_context.agent_name) == 0);
    assert(strcmp(lv_label_get_text(home.action), "Conversations") == 0);
    home_context.preview = true;
    dial_home_render(&home, &online_status, false, NULL, &home_context);
    assert(home.destination == DIAL_HOME_PREVIEW);
    assert(strcmp(lv_label_get_text(home.action), "Continue") == 0);
    lv_point_t action_size;
    lv_text_get_size(&action_size, lv_label_get_text(home.action),
        lv_obj_get_style_text_font(home.action, 0), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    assert(action_size.x <= lv_obj_get_width(home.action));
    assert(action_size.y <= lv_obj_get_height(home.action));
    snapshot("home-continue");
    dial_home_render(&home, &gateway_offline, false, NULL, &home_context);
    assert(home.destination == DIAL_HOME_CONNECTION);
    assert(strcmp(lv_label_get_text(home.action), "Connection") == 0);
    snapshot("home-offline");
    home_context = (dial_home_context_t){0};
    dial_home_render(&home, &online_status, false, NULL, &home_context);
    assert(home.destination == DIAL_HOME_AGENTS);
    snapshot("home-choose");
    lv_obj_update_layout(screen);
    check_home_bounds(home.title);
    lv_obj_t *arc = lv_arc_create(screen);
    lv_obj_set_size(arc, 218, 218);
    lv_obj_center(arc);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 50);
    lv_indev_t *encoder = lv_indev_create();
    lv_indev_set_type(encoder, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_mode(encoder, LV_INDEV_MODE_EVENT); // BSP's original mode.
    lv_indev_set_read_cb(encoder, encoder_read);
    pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, pointer_read);
    dial_input_init(&input, encoder, screen, arc, event, NULL);
    dial_input_bind_home(&input, home.menu_button, home.agent_button, home.primary_button);
    advance(40);

    // Turning must still adjust the value after confirming once.
    control();
    encoder_diff = 1;
    lv_indev_read(encoder);
    advance(40);
    assert(lv_arc_get_value(arc) == 51);
    encoder_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(encoder);
    advance(40);
    encoder_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(encoder);
    advance(40);
    assert(confirmations == 1);
    encoder_diff = 1;
    lv_indev_read(encoder);
    advance(40);
    assert(lv_arc_get_value(arc) == 52);
    puts("turn-confirm-turn: passed");

    // A stationary hold returns home and must never submit on release.
    encoder_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(encoder);
    advance(40);
    advance(1000);
    assert(!input.control && homes == 1);
    encoder_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(encoder);
    advance(40);
    assert(confirmations == 1);
    puts("stationary encoder hold: passed");

    // Returning from the face must restore a usable control.
    encoder_diff = 1;
    lv_indev_read(encoder);
    advance(40);
    assert(input.control);

    // Both short and sustained arc drags adjust only, never confirm or go home.
    control();
    unsigned before_changes = changes;
    touch(215, 120, true);
    touch(120, 25, true);
    touch(120, 25, false);
    assert(changes > before_changes && input.control);
    assert(confirmations == 1 && homes == 1);
    control();
    touch(215, 120, true);
    touch(120, 25, true);
    advance(1000);
    assert(input.control && homes == 1);
    touch(120, 25, false);
    assert(confirmations == 1);
    puts("touch drags: passed");

    // Stationary touches retain the documented tap and hold actions.
    touch(120, 120, true);
    touch(120, 120, false);
    assert(confirmations == 2);
    touch(120, 120, true);
    advance(1000);
    assert(!input.control && homes == 2);
    touch(120, 120, false);
    assert(confirmations == 2);
    puts("stationary touch tap/hold: passed");

    // A remotely shown/reconfigured control needs a fresh press to confirm.
    for (unsigned i = 0; i < 2; ++i) {
        encoder_state = LV_INDEV_STATE_PRESSED;
        lv_indev_read(encoder);
        advance(40);
        control();
        encoder_state = LV_INDEV_STATE_RELEASED;
        lv_indev_read(encoder);
        advance(40);
        assert(confirmations == 2);
    }
    encoder_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(encoder);
    advance(40);
    encoder_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(encoder);
    advance(40);
    assert(confirmations == 3);
    puts("control changes cancel held encoder presses: passed");

    // A remote view change also consumes an in-progress touch gesture.
    touch(120, 120, true);
    lv_obj_set_hidden(input.arc, true);
    lv_obj_set_hidden(home.root, false);
    dial_input_set_control(&input, false);
    touch(120, 120, false);
    assert(!input.control && confirmations == 3);
    touch(120, 201, true);
    touch(120, 201, false);
    assert(input.control && confirmations == 3);
    touch(120, 120, true);
    control();
    touch(120, 120, false);
    assert(confirmations == 3);
    touch(120, 120, true);
    touch(120, 120, false);
    assert(confirmations == 4);
    puts("view changes cancel held touches: passed");

    // The same real encoder now moves focus in both directions, and a press
    // opens the focused row without sending a numeric action.
    dial_navigation_create(&navigation, screen, navigation_event, NULL);
    navigating = true;
    lv_obj_set_hidden(arc, true);
    lv_obj_set_hidden(home.root, true);
    dial_navigation_open(&navigation, false);
    advance(40);
    encoder_diff = 1;
    lv_indev_read(encoder);
    advance(40);
    assert(next > 0 && navigation.focus == 1);
    encoder_diff = -1;
    lv_indev_read(encoder);
    advance(40);
    assert(previous > 0 && navigation.focus == 0);
    touch(120, 82, true);
    touch(150, 95, true);
    touch(150, 95, false);
    assert(navigation.view == DIAL_NAV_MENU && refreshes == 0);
    touch(120, 82, true);
    touch(120, 82, false);
    assert(navigation.view == DIAL_NAV_AGENTS && refreshes == 1);
    roster = (dial_agents_snapshot_t){.state = DIAL_AGENTS_READY, .revision = 1,
        .page = {.count = 8, .total = 10}};
    for (unsigned i = 0; i < 8; ++i) {
        snprintf(roster.page.agents[i].id, 65, "agent-%u", i);
        snprintf(roster.page.agents[i].name, 65, "Assistant %u", i);
    }
    strcpy(roster.page.agents[0].name, "A very long assistant name that needs truncation");
    strcpy(roster.page.agents[0].emoji, "🦊");
    dial_navigation_update(&navigation, &roster);
    advance(40);
    for (unsigned i = 0; i < lv_obj_get_child_count(navigation.root); ++i)
        check_home_bounds(lv_obj_get_child(navigation.root, i));
    uint32_t fox = avatar_hash(46, 70, 24);
    snapshot("agents-fox");
    strcpy(roster.page.agents[0].emoji, "🦞");
    ++roster.revision;
    dial_navigation_update(&navigation, &roster);
    assert(avatar_hash(46, 70, 24) != fox);
    snapshot("agents-lobster");
    // The image must pass pointer input to its owning row.
    touch(58, 82, true);
    touch(58, 82, false);
    assert(navigation.view == DIAL_NAV_DETAIL);
    uint32_t lobster = avatar_hash(100, 64, 40);
    snapshot("agent-lobster");
    strcpy(roster.page.agents[0].emoji, "🦊");
    ++roster.revision;
    dial_navigation_update(&navigation, &roster);
    assert(navigation.view == DIAL_NAV_AGENTS);
    touch(58, 82, true);
    touch(58, 82, false);
    assert(avatar_hash(100, 64, 40) != lobster);
    snapshot("agent-fox");
    strcpy(roster.page.agents[0].emoji, "🔮");
    ++roster.revision;
    dial_navigation_update(&navigation, &roster);
    assert(navigation.view == DIAL_NAV_AGENTS);
    touch(58, 82, true);
    touch(58, 82, false);
    assert(lv_obj_is_visible(navigation.detail_initials));
    assert(!lv_obj_is_visible(navigation.detail_avatar));
    assert(strcmp(lv_label_get_text(navigation.detail_initials), "AV") == 0);
    snapshot("agent-initials");
    strcpy(roster.page.agents[0].emoji, "🦊");
    ++roster.revision;
    dial_navigation_update(&navigation, &roster);
    touch(58, 82, true);
    touch(58, 82, false);
    assert(strcmp(lv_label_get_text(navigation.identity), "agent-0") == 0);
    touch(68, 44, true);
    touch(68, 44, false);
    assert(navigation.view == DIAL_NAV_AGENTS && navigation.focus == 0);

    // Both row and Back presses belong to the rendered revision. The shared
    // input cancellation must consume their release after the roster changes.
    touch(120, 82, true);
    ++roster.revision;
    strcpy(roster.page.agents[0].id, "replacement");
    dial_navigation_update(&navigation, &roster);
    touch(120, 82, false);
    assert(navigation.view == DIAL_NAV_AGENTS);
    touch(68, 44, true);
    ++roster.revision;
    dial_navigation_update(&navigation, &roster);
    touch(68, 44, false);
    assert(navigation.view == DIAL_NAV_AGENTS);

    encoder_diff = 8;
    lv_indev_read(encoder);
    advance(40);
    assert(navigation.focus == 8);
    encoder_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(encoder);
    advance(40);
    encoder_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(encoder);
    advance(40);
    assert(refreshes == 2 && requested_offset == 8);

    // A replacement/offline roster invalidates an in-progress press. Releasing
    // it cannot open the new first row or send a stale control action.
    encoder_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(encoder);
    advance(40);
    roster.state = DIAL_AGENTS_OFFLINE;
    ++roster.revision;
    dial_navigation_update(&navigation, &roster);
    encoder_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(encoder);
    advance(40);
    assert(refreshes == 2 && navigation.view == DIAL_NAV_AGENTS);
    assert(strstr(lv_label_get_text(navigation.description), "Gateway offline") != NULL);
    for (unsigned i = 0; i < lv_obj_get_child_count(navigation.root); ++i)
        check_home_bounds(lv_obj_get_child(navigation.root, i));
    encoder_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(encoder);
    advance(1000);
    assert(!navigating && !input.control);
    encoder_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(encoder);
    advance(40);
    assert(confirmations == 4);
    puts("agent navigation, paging, bounds and stale presses: passed");

    // An empty end page still exposes Previous, Refresh and Back on the two
    // available rows; it must not strand the encoder on a hidden third row.
    navigating = true;
    lv_obj_set_hidden(home.root, true);
    dial_navigation_open(&navigation, true);
    roster = (dial_agents_snapshot_t){.state = DIAL_AGENTS_EMPTY, .revision = 3,
        .page = {.offset = 8, .total = 8}};
    dial_navigation_update(&navigation, &roster);
    dial_navigation_move(&navigation, 1);
    dial_navigation_move(&navigation, 1);
    assert(strcmp(lv_label_get_text(navigation.names[0]), "Back") == 0);
    assert(!lv_obj_has_flag(navigation.rows[0], LV_OBJ_FLAG_HIDDEN));
    dial_navigation_activate(&navigation);
    assert(navigation.view == DIAL_NAV_MENU);
    puts("empty-page navigation: passed");

    roster.state = DIAL_AGENTS_READY;
    roster.revision = 4;
    roster.page.count = roster.page.total = 1;
    roster.page.offset = 0;
    strcpy(roster.page.agents[0].id, "beta");
    strcpy(roster.page.agents[0].name, "Beta");
    dial_navigation_update(&navigation, &roster);
    dial_navigation_open(&navigation, true);
    touch(120, 82, true); touch(120, 82, false);
    assert(navigation.view == DIAL_NAV_DETAIL);
    snapshot("agent");
    touch(120, 172, true); touch(120, 172, false);
    assert(navigation.view == DIAL_NAV_SESSIONS && opens == 1 && requested_offset == 0);
    conversations = (dial_conversations_snapshot_t){.state = DIAL_AGENTS_LOADING, .revision = 1};
    conversations.agent = roster.page.agents[0];
    dial_navigation_conversations_update(&navigation, &conversations);
    assert(strstr(lv_label_get_text(navigation.description), "Loading conversations") != NULL);
    conversations.state = DIAL_AGENTS_READY;
    conversations.page.count = 2;
    conversations.page.has_more = true;
    conversations.page.next_offset = 2;
    strcpy(conversations.page.sessions[0].title, "Planning the next adventure");
    strcpy(conversations.page.sessions[1].title, "A question for Beta");
    dial_navigation_conversations_update(&navigation, &conversations);
    snapshot("list");
    assert(strncmp(lv_label_get_text(navigation.names[0]), "Planning", 8) == 0);
    touch(120, 82, true);
    ++conversations.revision;
    dial_navigation_conversations_update(&navigation, &conversations);
    touch(120, 82, false);
    assert(navigation.view == DIAL_NAV_SESSIONS && reads == 0);
    dial_navigation_move(&navigation, 1);
    dial_navigation_activate(&navigation);
    assert(navigation.view == DIAL_NAV_PREVIEW && reads == 1 && requested_offset == 1);
    conversations.selected = 1;
    conversations.preview_state = DIAL_AGENTS_LOADING;
    ++conversations.preview_revision;
    dial_navigation_conversations_update(&navigation, &conversations);
    assert(strstr(lv_label_get_text(navigation.preview_text), "Loading") != NULL);
    conversations.preview_state = DIAL_AGENTS_READY;
    conversations.preview.status = ESP_OPENCLAW_PREVIEW_OK;
    conversations.preview.count = 2;
    conversations.preview.items[0].role = ESP_OPENCLAW_PREVIEW_USER;
    strcpy(conversations.preview.items[0].text, "What should we work on next?");
    conversations.preview.items[1].role = ESP_OPENCLAW_PREVIEW_ASSISTANT;
    strcpy(conversations.preview.items[1].text, "Start with a small working journey. Pick an agent, read a conversation, and return without losing your place. Then check how the controls behave when the network drops and reconnects.");
    dial_navigation_conversations_update(&navigation, &conversations);
    snapshot("preview");
    assert(strstr(lv_label_get_text(navigation.preview_text), "What should we work on next?") != NULL);
    dial_navigation_move(&navigation, 1);
    advance(40);
    assert(lv_obj_get_scroll_y(navigation.preview_body) > 0);
    snapshot("scrolled");
    check_reader_limits(encoder, "preview-bottom");
    // Home and the menu retain the last read row without another list request.
    unsigned before_resume = refreshes;
    encoder_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(encoder); advance(1000);
    encoder_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(encoder); advance(40);
    assert(!navigating);
    navigating = true;
    lv_obj_set_hidden(home.root, true);
    dial_navigation_open(&navigation, false);
    snapshot("resume-menu");
    touch(120, 126, true); touch(120, 126, false);
    assert(navigation.view == DIAL_NAV_SESSIONS && navigation.focus == 1);
    assert(refreshes == before_resume && confirmations == 4);
    assert(strcmp(lv_label_get_text(navigation.heading), "Beta") == 0);
    snapshot("resumed");
    // Selection indexes the returned page, even when that page has an offset.
    conversations.page.offset = 8;
    conversations.page.next_offset = 10;
    ++conversations.revision;
    dial_navigation_conversations_update(&navigation, &conversations);
    dial_navigation_open(&navigation, false);
    touch(120, 126, true); touch(120, 126, false);
    assert(navigation.view == DIAL_NAV_SESSIONS && navigation.focus == 1);
    assert(strncmp(lv_label_get_text(navigation.names[1]), "A quest", 7) == 0);
    assert(strcmp(conversations.page.sessions[navigation.focus].title, "A question for Beta") == 0);
    dial_navigation_activate(&navigation);
    assert(navigation.view == DIAL_NAV_PREVIEW && requested_offset == 1);
    touch(68, 44, true); touch(68, 44, false);
    assert(navigation.view == DIAL_NAV_SESSIONS && navigation.focus == 1);
    dial_navigation_activate(&navigation);
    conversations.preview.count = 0;
    conversations.preview.status = ESP_OPENCLAW_PREVIEW_MISSING;
    ++conversations.preview_revision;
    dial_navigation_conversations_update(&navigation, &conversations);
    assert(strcmp(lv_label_get_text(navigation.preview_text), "Conversation unavailable") == 0);
    snapshot("missing");
    conversations.preview_state = conversations.state = DIAL_AGENTS_OFFLINE;
    dial_navigation_conversations_update(&navigation, &conversations);
    assert(strstr(lv_label_get_text(navigation.preview_text), "Gateway offline") != NULL);
    assert(strstr(lv_label_get_text(navigation.preview_text), "What should") == NULL);
    touch(68, 44, true); touch(68, 44, false);
    touch(68, 44, true); touch(68, 44, false);
    assert(navigation.view == DIAL_NAV_DETAIL && navigation.focus == 0 && confirmations == 4);
    puts("conversation list, scrolling, missing/offline preview and return focus: passed");

    // A rejected reader command after Home must leave the active control usable.
    navigation.view = DIAL_NAV_PREVIEW;
    navigating = false;
    lv_obj_set_hidden(navigation.root, true);
    control();
    unsigned before_rejection = changes;
    conversations = (dial_conversations_snapshot_t){.revision = 99};
    assert(!dial_navigation_show_reader(&navigation, &roster, &conversations, true));
    encoder_diff = 1;
    lv_indev_read(input.encoder);
    advance(40);
    assert(changes == before_rejection + 1);
    puts("rejected reader page preserves active control input: passed");

    // An invalidated selection routes back to agent selection, never old rows.
    navigating = true;
    lv_obj_set_hidden(home.root, true);
    lv_obj_set_hidden(arc, true);
    dial_navigation_open(&navigation, false);
    before_resume = refreshes;
    touch(120, 126, true); touch(120, 126, false);
    assert(navigation.view == DIAL_NAV_AGENTS && refreshes == before_resume + 1);
    assert(confirmations == 4);
    puts("conversation resume and invalidated selection: passed");

    // Local settings use real encoder/touch input and never send the numeric
    // control. Network refreshes must not replace this local page.
    navigating = true;
    lv_obj_set_hidden(home.root, true);
    lv_obj_set_hidden(arc, true);
    dial_input_set_control(&input, false);
    dial_navigation_set_brightness(&navigation, brightness, false);
    dial_navigation_open(&navigation, false);
    for (unsigned i = 0; i < 4; ++i) {
        encoder_diff = 1;
        lv_indev_read(encoder); advance(40);
    }
    snapshot("menu-settings");
    assert(strcmp(lv_label_get_text(navigation.names[0]), "Devices") == 0);
    assert(strcmp(lv_label_get_text(navigation.names[1]), "Activity") == 0);
    assert(strcmp(lv_label_get_text(navigation.names[2]), "Settings") == 0);
    touch(120, 168, true); touch(120, 168, false);
    assert(navigation.view == DIAL_NAV_SETTINGS);
    snapshot("settings");
    touch(120, 82, true); touch(120, 82, false);
    assert(navigation.view == DIAL_NAV_DISPLAY);
    snapshot("display-menu");
    touch(120, 82, true); touch(120, 82, false);
    assert(navigation.view == DIAL_NAV_BACKLIGHT);
    assert(strcmp(lv_label_get_text(navigation.description), "65%") == 0);
    snapshot("display");
    lv_obj_update_layout(screen);
    for (unsigned i = 0; i < lv_obj_get_child_count(navigation.root); ++i)
        check_home_bounds(lv_obj_get_child(navigation.root, i));
    unsigned before_settings = confirmations;
    encoder_diff = 1;
    lv_indev_read(encoder); advance(40);
    assert(brightness == 70 && requested_offset == 70);
    assert(strcmp(lv_label_get_text(navigation.description), "70%") == 0);
    touch(75, 168, true); touch(75, 168, false);
    assert(brightness == 65);
    touch(165, 168, true); touch(185, 168, true); touch(185, 168, false);
    assert(brightness == 65);
    touch(165, 168, true); touch(165, 168, false);
    assert(brightness == 70);
    brightness_failure = true;
    encoder_diff = 1;
    lv_indev_read(encoder); advance(40);
    assert(brightness == 70 && requested_offset == 75);
    assert(strcmp(lv_label_get_text(navigation.description), "70%") == 0);
    assert(strcmp(lv_label_get_text(navigation.identity), "Couldn't change") == 0);
    snapshot("display-failed");
    brightness_failure = false;
    encoder_diff = 1;
    lv_indev_read(encoder); advance(40);
    assert(brightness == 75 && !navigation.brightness_failed);
    roster.state = DIAL_AGENTS_OFFLINE;
    ++roster.revision;
    dial_navigation_update(&navigation, &roster);
    assert(navigation.view == DIAL_NAV_BACKLIGHT && brightness == 75);
    for (unsigned i = 0; i < 30; ++i) dial_navigation_move(&navigation, 1);
    assert(brightness == 100);
    unsigned before_limit = brightness_requests;
    dial_navigation_move(&navigation, 1);
    assert(brightness_requests == before_limit);
    for (unsigned i = 0; i < 30; ++i) dial_navigation_move(&navigation, -1);
    assert(brightness == 10);
    dial_navigation_move(&navigation, -1);
    assert(brightness == 10 && confirmations == before_settings);
    // A command's new brightness cancels a held button; release cannot apply
    // the old gesture to its replacement value.
    touch(165, 168, true);
    brightness = 55;
    dial_navigation_set_brightness(&navigation, brightness, false);
    touch(165, 168, false);
    assert(brightness == 55);
    touch(68, 44, true); touch(68, 44, false);
    assert(navigation.view == DIAL_NAV_DISPLAY && navigation.focus == 0);
    touch(68, 44, true); touch(68, 44, false);
    assert(navigation.view == DIAL_NAV_SETTINGS && navigation.focus == 0);
    ++roster.revision;
    dial_navigation_update(&navigation, &roster);
    assert(navigation.view == DIAL_NAV_SETTINGS);
    // Display uses the same row objects: returning must restore normal touch
    // locations, including the Display row and Back arrow.
    touch(120, 82, true); touch(120, 82, false);
    assert(navigation.view == DIAL_NAV_DISPLAY);
    touch(68, 44, true); touch(68, 44, false);
    touch(120, 126, true); touch(120, 126, false);
    assert(navigation.view == DIAL_NAV_CONNECTION);
    esp_openclaw_node_wifi_status_t wifi = {0};
    dial_navigation_set_connection(&navigation, &wifi, false);
    assert(strstr(lv_label_get_text(navigation.preview_text), "Wi-Fi not set up") != NULL);
    assert(strstr(lv_label_get_text(navigation.preview_text), "Phone pairing is not available yet") != NULL);
    snapshot("connection-unconfigured");
    wifi.has_saved_network = true;
    strcpy(wifi.ssid, "Studio Wi-Fi");
    wifi.last_disconnect_reason = 201;
    dial_navigation_set_connection(&navigation, &wifi, false);
    assert(strstr(lv_label_get_text(navigation.preview_text), "Saved network\nStudio Wi-Fi") != NULL);
    assert(strstr(lv_label_get_text(navigation.preview_text), "Disconnect code: 201") != NULL);
    snapshot("connection-offline");
    unsigned connection_requests = brightness_requests;
    encoder_diff = 1;
    lv_indev_read(encoder); advance(40);
    assert(lv_obj_get_scroll_y(navigation.preview_body) > 0);
    int32_t scrolled = lv_obj_get_scroll_y(navigation.preview_body);
    dial_navigation_set_connection(&navigation, &wifi, false);
    assert(lv_obj_get_scroll_y(navigation.preview_body) == scrolled);
    touch(120, 98, true); touch(120, 98, false);
    assert(navigation.view == DIAL_NAV_CONNECTION && brightness_requests == connection_requests);
    assert(confirmations == before_settings);
    wifi.connected = true;
    strcpy(wifi.ip, "192.0.2.10");
    dial_navigation_set_connection(&navigation, &wifi, false);
    assert(strstr(lv_label_get_text(navigation.preview_text), "Signal\nUnavailable") != NULL);
    assert(strstr(lv_label_get_text(navigation.preview_text), "IP address\n192.0.2.10") != NULL);
    assert(strstr(lv_label_get_text(navigation.preview_text), "Gateway offline") != NULL);
    lv_obj_scroll_to_y(navigation.preview_body, 0, LV_ANIM_OFF);
    snapshot("connection-wifi");
    wifi.ap_info_valid = true;
    wifi.rssi = -63;
    wifi.channel = 6;
    dial_navigation_set_connection(&navigation, &wifi, true);
    assert(strstr(lv_label_get_text(navigation.preview_text), "-63 dBm / ch 6") != NULL);
    assert(strstr(lv_label_get_text(navigation.preview_text), "Gateway connected") != NULL);
    snapshot("connection-online");
    for (unsigned i = 0; i < 40; ++i) dial_navigation_move(&navigation, 1);
    assert(lv_obj_get_scroll_bottom(navigation.preview_body) >= 0);
    advance(40);
    assert(lv_obj_get_scroll_y(navigation.preview_body) > 0);
    snapshot("connection-gateway");
    wifi.rssi = 0;
    dial_navigation_set_connection(&navigation, &wifi, true);
    assert(strstr(lv_label_get_text(navigation.preview_text), "0 dBm / ch 6") != NULL);
    wifi.ap_info_valid = false;
    dial_navigation_set_connection(&navigation, &wifi, true);
    assert(strstr(lv_label_get_text(navigation.preview_text), "0 dBm") == NULL);
    assert(strstr(lv_label_get_text(navigation.preview_text), "Signal\nUnavailable") != NULL);
    wifi.connected = false;
    dial_navigation_set_connection(&navigation, &wifi, false);
    assert(strstr(lv_label_get_text(navigation.preview_text), "192.0.2.10") == NULL);
    assert(strstr(lv_label_get_text(navigation.preview_text), "dBm") == NULL);
    strcpy(wifi.ssid, "Caf\xc3\xa9\n\xff");
    dial_navigation_set_connection(&navigation, &wifi, false);
    assert(strstr(lv_label_get_text(navigation.preview_text), "Caf\xc3\xa9??") != NULL);
    memset(wifi.ssid, 'A', 32);
    wifi.ssid[31] = (char)0xf0;
    wifi.ssid[32] = '\0';
    dial_navigation_set_connection(&navigation, &wifi, false);
    assert(strstr(lv_label_get_text(navigation.preview_text), "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA?") != NULL);
    lv_obj_update_layout(screen);
    for (unsigned i = 0; i < lv_obj_get_child_count(navigation.root); ++i)
        check_home_bounds(lv_obj_get_child(navigation.root, i));
    ++roster.revision;
    dial_navigation_update(&navigation, &roster);
    assert(navigation.view == DIAL_NAV_CONNECTION);
    // A status update does not change the meaning of an already-held Back.
    touch(68, 44, true);
    dial_navigation_set_connection(&navigation, &wifi, false);
    touch(68, 44, false);
    assert(navigation.view == DIAL_NAV_SETTINGS && navigation.focus == 1);
    touch(120, 82, true); touch(120, 82, false);
    assert(navigation.view == DIAL_NAV_DISPLAY);
    touch(68, 44, true); touch(68, 44, false);
    touch(68, 44, true); touch(68, 44, false);
    assert(navigation.view == DIAL_NAV_MENU);
    dial_navigation_open_settings(&navigation, DIAL_NAV_DISPLAY);
    dial_navigation_move(&navigation, 1);
    dial_navigation_move(&navigation, 1);
    dial_navigation_move(&navigation, 1);
    encoder_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(encoder); advance(40);
    encoder_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(encoder); advance(40);
    assert(navigation.view == DIAL_NAV_SETTINGS);
    touch(120, 126, true); touch(120, 126, false);
    assert(navigation.view == DIAL_NAV_CONNECTION);
    encoder_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(encoder); advance(1000);
    encoder_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(encoder); advance(40);
    assert(!navigating && confirmations == before_settings);
    puts("local display and connection input, bounds, failures, offline focus and return: passed");
    check_messaging(encoder);
    check_replies(encoder);
    check_creation(encoder);

    /* Read the latest request through real menu/encoder/touch input. Browsing
     * and result updates never confirm the numeric control. */
    navigating = true;
    lv_obj_set_hidden(home.root, true);
    dial_navigation_open(&navigation, false);
    for (unsigned i = 0; i < 3; ++i) dial_navigation_move(&navigation, 1);
    assert(strcmp(lv_label_get_text(navigation.names[1]), "Activity") == 0);
    touch(120, 126, true); touch(120, 126, false);
    assert(navigation.view == DIAL_NAV_ACTIVITY);
    assert(strcmp(lv_label_get_text(navigation.description), "No requests yet") == 0);
    snapshot("activity-empty");
    dial_action_state_t action_state = {0};
    dial_action_token_t token;
    assert(dial_action_connect(&action_state, 1, "ws://example.test"));
    assert(dial_action_configure(&action_state, true));
    assert(dial_action_begin(&action_state, &token));
    dial_action_submitted(&action_state, token, "Desk light", 45, "%");
    dial_navigation_activity_update(&navigation, &action_state.activity);
    assert(strcmp(lv_label_get_text(navigation.description), "Sending") == 0);
    assert(strstr(lv_label_get_text(navigation.preview_text), "Desk light\n45%") != NULL);
    snapshot("activity-sending");
    assert(dial_action_complete(&action_state, token, DIAL_ACTION_ACCEPTED));
    dial_navigation_activity_update(&navigation, &action_state.activity);
    assert(strcmp(lv_label_get_text(navigation.description), "Waiting for result") == 0);
    snapshot("activity-waiting");
    encoder_diff = 1;
    lv_indev_read(encoder); advance(40);
    lv_coord_t reading_at = lv_obj_get_scroll_y(navigation.preview_body);
    assert(reading_at > 0);
    dial_navigation_activity_update(&navigation, &action_state.activity);
    advance(40);
    assert(lv_obj_get_scroll_y(navigation.preview_body) == reading_at);
    check_reader_limits(encoder, "activity-bottom");
    // A new result cancels a held Back and preserves the reading position.
    touch(68, 44, true);
    assert(dial_action_resolve(&action_state, token, true,
        "The desk light is now at 45%. This long result remains readable by turning the dial or scrolling the detail."));
    dial_navigation_activity_update(&navigation, &action_state.activity);
    touch(68, 44, false);
    assert(navigation.view == DIAL_NAV_ACTIVITY);
    assert(lv_obj_get_scroll_y(navigation.preview_body) == reading_at);
    assert(strcmp(lv_label_get_text(navigation.description), "Completed") == 0);
    assert(strstr(lv_label_get_text(navigation.preview_text), "45%") != NULL);
    snapshot("activity-completed");
    dial_navigation_move(&navigation, -1);
    snapshot("activity-top");
    touch(120, 150, true); touch(120, 112, true); touch(120, 80, true); touch(120, 80, false);
    assert(navigation.view == DIAL_NAV_ACTIVITY);
    encoder_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(encoder); advance(40);
    encoder_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(encoder); advance(40);
    assert(navigation.view == DIAL_NAV_MENU && navigation.focus == 3);
    assert(confirmations == before_settings);
    // Completion off-screen is visible on re-entry, and disconnect retains it.
    assert(dial_action_connect(&action_state, 0, NULL));
    dial_navigation_activity_update(&navigation, &action_state.activity);
    touch(120, 126, true); touch(120, 126, false);
    assert(navigation.view == DIAL_NAV_ACTIVITY);
    assert(strcmp(lv_label_get_text(navigation.description), "Completed") == 0);
    assert(dial_action_connect(&action_state, 2, "ws://example.test"));
    assert(dial_action_begin(&action_state, &token));
    dial_action_submitted(&action_state, token, "Desk light", 60, "%");
    assert(dial_action_complete(&action_state, token, DIAL_ACTION_UNCERTAIN));
    dial_navigation_activity_update(&navigation, &action_state.activity);
    assert(strcmp(lv_label_get_text(navigation.description), "Outcome unknown") == 0);
    assert(strstr(lv_label_get_text(navigation.preview_text), "Check before retry") != NULL);
    snapshot("activity-unknown");
    assert(dial_action_resolve(&action_state, token, false, "The light is unavailable."));
    dial_navigation_activity_update(&navigation, &action_state.activity);
    assert(strcmp(lv_label_get_text(navigation.description), "Failed") == 0);
    snapshot("activity-failed");
    // Empty result text is valid, including when completion beats the ack.
    for (unsigned success = 0; success < 2; ++success) {
        assert(dial_action_begin(&action_state, &token));
        dial_action_submitted(&action_state, token, "Desk light", 50, "%");
        assert(dial_action_resolve(&action_state, token, success != 0, ""));
        dial_navigation_activity_update(&navigation, &action_state.activity);
        assert(strstr(lv_label_get_text(navigation.preview_text), success ? "reported success" : "reported failure") != NULL);
        assert(strstr(lv_label_get_text(navigation.preview_text), "Sending") == NULL);
        assert(!dial_action_complete(&action_state, token, DIAL_ACTION_ACCEPTED));
        snapshot(success ? "activity-empty-success" : "activity-empty-failure");
    }
    lv_obj_update_layout(screen);
    for (unsigned i = 0; i < lv_obj_get_child_count(navigation.root); ++i)
        check_home_bounds(lv_obj_get_child(navigation.root, i));
    encoder_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(encoder); advance(1000);
    encoder_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(encoder); advance(40);
    assert(!navigating && confirmations == before_settings);
    assert(dial_action_connect(&action_state, 0, NULL));
    assert(dial_action_configure(&action_state, false));
    puts("Activity outcomes, scrolling, retained values, stale holds and safe return: passed");

    navigating = true;
    lv_obj_set_hidden(home.root, true);
    dial_navigation_open(&navigation, false);
    for (unsigned i = 0; i < 2; ++i) {
        encoder_diff = 1; lv_indev_read(encoder); advance(40);
    }
    touch(120, 168, true); touch(120, 168, false);
    assert(navigation.view == DIAL_NAV_DEVICES && device_refreshes == 1);
    devices.state = ESP_OPENCLAW_DEVICES_READY;
    devices.revision = 1;
    devices.count = 5;
    for (unsigned i = 0; i < devices.count; ++i) {
        snprintf(devices.devices[i].name, sizeof(devices.devices[i].name), "Studio device %u", i + 1);
        strcpy(devices.devices[i].model, "A long device model that remains readable while turning the dial");
    }
    devices.devices[0].connected_known = devices.devices[0].connected = true;
    devices.devices[3].paired_known = devices.devices[3].connected_known = true;
    dial_navigation_devices_update(&navigation, &devices);
    assert(strcmp(lv_label_get_text(navigation.names[0]), "This Dial") == 0);
    assert(strcmp(lv_label_get_text(navigation.icons[1]), LV_SYMBOL_OK) == 0);
    assert(strcmp(lv_label_get_text(navigation.icons[2]), "?") == 0);
    snapshot("devices");
    touch(120, 126, true); touch(120, 126, false);
    assert(navigation.view == DIAL_NAV_DEVICE_DETAIL);
    assert(strstr(lv_label_get_text(navigation.preview_text), "Studio device 1") != NULL);
    assert(strstr(lv_label_get_text(navigation.preview_text), "Connection: Online") != NULL);
    assert(strstr(lv_label_get_text(navigation.preview_text), "Pairing: Unknown") != NULL);
    snapshot("device-detail");
    check_reader_limits(encoder, "device-bottom");
    lv_obj_update_layout(screen);
    for (unsigned i = 0; i < lv_obj_get_child_count(navigation.root); ++i)
        check_home_bounds(lv_obj_get_child(navigation.root, i));
    touch(68, 44, true); touch(68, 44, false);
    assert(navigation.view == DIAL_NAV_DEVICES && navigation.focus == 1);
    for (unsigned i = 0; i < 3; ++i) { encoder_diff = 1; lv_indev_read(encoder); advance(40); }
    assert(navigation.focus == 4 && navigation.first == 3 && device_refreshes == 1);
    touch(120, 126, true); touch(120, 126, false);
    assert(navigation.view == DIAL_NAV_DEVICE_DETAIL);
    assert(strstr(lv_label_get_text(navigation.preview_text), "Connection: Offline") != NULL);
    assert(strstr(lv_label_get_text(navigation.preview_text), "Pairing: Not paired") != NULL);
    touch(68, 44, true); touch(68, 44, false);
    assert(navigation.focus == 4 && navigation.first == 3);
    for (unsigned i = 0; i < 2; ++i) { encoder_diff = 1; lv_indev_read(encoder); advance(40); }
    touch(120, 82, true); touch(120, 82, false);
    assert(device_refreshes == 2);
    ++devices.revision;
    dial_navigation_devices_update(&navigation, &devices);
    touch(120, 126, true);
    ++devices.revision;
    strcpy(devices.devices[0].name, "Replacement row");
    dial_navigation_devices_update(&navigation, &devices);
    touch(120, 126, false);
    assert(navigation.view == DIAL_NAV_DEVICES);
    devices.state = ESP_OPENCLAW_DEVICES_OFFLINE; ++devices.revision;
    dial_navigation_devices_update(&navigation, &devices);
    assert(navigation.count == 3 && strcmp(lv_label_get_text(navigation.hint), "Offline / local") == 0);
    snapshot("devices-offline");
    touch(120, 82, true); touch(120, 82, false);
    assert(navigation.view == DIAL_NAV_LOCAL_DEVICE);
    snapshot("this-dial");
    touch(120, 126, true); touch(120, 126, false);
    assert(navigation.view == DIAL_NAV_DISPLAY);
    touch(68, 44, true); touch(68, 44, false);
    assert(navigation.view == DIAL_NAV_LOCAL_DEVICE && navigation.focus == 1);
    touch(120, 168, true); touch(120, 168, false);
    assert(navigation.view == DIAL_NAV_CONNECTION);
    touch(68, 44, true); touch(68, 44, false);
    assert(navigation.view == DIAL_NAV_LOCAL_DEVICE && navigation.focus == 2);
    touch(68, 44, true); touch(68, 44, false);
    assert(navigation.view == DIAL_NAV_DEVICES && device_refreshes == 3);
    touch(68, 44, true); touch(68, 44, false);
    assert(navigation.view == DIAL_NAV_MENU && navigation.focus == 2);
    assert(confirmations == before_settings);
    puts("Device inventory: paging, details, stale input, offline controls and Back passed");

    dial_navigation_open_local_device(&navigation);
    touch(120, 82, true); touch(120, 82, false);
    assert(input.control && !navigating);
    assert(!lv_obj_is_hidden(input.back));
    lv_obj_update_layout(screen);
    check_home_bounds(input.back);
    snapshot("control-back");
    touch(68, 44, true); touch(68, 44, false);
    assert(navigating && !input.control && lv_obj_is_hidden(input.back));
    assert(navigation.view == DIAL_NAV_LOCAL_DEVICE && navigation.focus == 0);
    assert(confirmations == before_settings);
    touch(120, 82, true); touch(120, 82, false);
    assert(input.control);
    touch(68, 44, true); touch(100, 44, true); touch(100, 44, false);
    assert(input.control && !navigating && confirmations == before_settings);
    touch(68, 44, true);
    dial_input_set_control(&input, true);
    touch(68, 44, false);
    assert(input.control && !navigating && confirmations == before_settings);
    unsigned before_back_home = homes;
    touch(68, 44, true); advance(1000); touch(68, 44, false);
    assert(!input.control && !navigating && homes == before_back_home + 1);
    assert(confirmations == before_settings);
    puts("Control Back: tap, drag, replaced press and hold preserve confirmation semantics");

    navigating = true;
    dial_input_set_control(&input, false);
    lv_obj_set_hidden(home.root, true);
    lv_obj_set_hidden(arc, true);
    dial_activity_t reading_activity = {.stage = DIAL_ACTIVITY_FAILED,
        .label = "Reading test", .message = "A longer response wraps inside the reading area. Turn to read the remaining words. Text size never submits a control request."};
    dial_navigation_activity_update(&navigation, &reading_activity);
    dial_navigation_open_activity(&navigation);
    lv_obj_update_layout(screen);
    int standard_height = lv_obj_get_height(navigation.preview_text);
    unsigned reading_confirmations = confirmations;
    dial_navigation_open_settings(&navigation, DIAL_NAV_DISPLAY);
    touch(120, 126, true); touch(120, 126, false);
    assert(navigation.view == DIAL_NAV_READING);
    touch(120, 126, true); touch(120, 126, false);
    assert(openclaw_reading_size() == OPENCLAW_READING_LARGE);
    lv_obj_update_layout(screen);
    for (unsigned i = 0; i < 3; ++i) {
        assert(lv_obj_get_height(navigation.rows[i]) >= 44);
        check_home_bounds(navigation.rows[i]);
        assert(lv_obj_get_style_text_font(navigation.names[i], 0) == &lv_font_montserrat_20);
    }
    snapshot("reading-settings");
    dial_navigation_open_activity(&navigation);
    lv_obj_update_layout(screen);
    assert(lv_obj_get_style_text_font(navigation.preview_text, 0) == &lv_font_montserrat_20);
    assert(lv_obj_get_height(navigation.preview_text) > standard_height);
    dial_navigation_move(&navigation, 1);
    assert(lv_obj_get_scroll_y(navigation.preview_body) > 0);
    snapshot("reading-large");
    dial_navigation_open_settings(&navigation, DIAL_NAV_DISPLAY);
    touch(120, 126, true); touch(120, 126, false);
    touch(120, 82, true); touch(120, 82, false);
    assert(openclaw_reading_size() == OPENCLAW_READING_STANDARD && confirmations == reading_confirmations);
    puts("Dial reading size: input, reflow, scrolling and no control submission passed");
    unsigned density_confirmations = confirmations;
    dial_navigation_open_settings(&navigation, DIAL_NAV_DISPLAY);
    dial_navigation_move(&navigation, 1);
    dial_navigation_move(&navigation, 1);
    dial_navigation_activate(&navigation);
    assert(navigation.view == DIAL_NAV_DENSITY);
    touch(120, 126, true); touch(120, 126, false);
    assert(openclaw_density() == OPENCLAW_DENSITY_COMFORTABLE && navigation.view == DIAL_NAV_DENSITY);
    lv_obj_update_layout(screen);
    assert(lv_obj_is_hidden(navigation.rows[2]));
    for (unsigned i = 0; i < 2; ++i) {
        assert(lv_obj_get_height(navigation.rows[i]) == 60);
        check_home_bounds(navigation.rows[i]);
        assert(lv_obj_get_height(navigation.names[i]) >=
            lv_obj_get_style_text_font(navigation.names[i], 0)->line_height);
    }
    assert(strcmp(lv_label_get_text(navigation.names[1]), "Comfortable") == 0);
    snapshot("density-comfortable");
    dial_navigation_move(&navigation, 1);
    assert(navigation.first == 2 && strcmp(lv_label_get_text(navigation.names[0]), "Back") == 0);
    dial_navigation_activate(&navigation);
    assert(navigation.view == DIAL_NAV_DISPLAY && navigation.focus == 2);
    dial_navigation_open(&navigation, false);
    dial_navigation_move(&navigation, 1);
    dial_navigation_move(&navigation, 1);
    dial_navigation_move(&navigation, 1);
    assert(navigation.focus == 3 && navigation.first == 2);
    assert(strcmp(lv_label_get_text(navigation.names[1]), "Activity") == 0);
    snapshot("density-menu");
    dial_navigation_open_settings(&navigation, DIAL_NAV_DISPLAY);
    dial_navigation_move(&navigation, 1); dial_navigation_move(&navigation, 1);
    dial_navigation_activate(&navigation);
    dial_navigation_move(&navigation, -1);
    dial_navigation_activate(&navigation);
    assert(openclaw_density() == OPENCLAW_DENSITY_COMPACT);
    assert(confirmations == density_confirmations && openclaw_reading_size() == OPENCLAW_READING_STANDARD);
    puts("Dial list spacing: two-row paging, Back focus, circular bounds and no control submission passed");
    navigating = false;
    lv_obj_set_hidden(navigation.root, true);
    lv_obj_set_hidden(input.arc, true);
    lv_obj_set_hidden(home.root, false);
    dial_input_set_control(&input, false);
    touch(120, 120, true); touch(120, 120, false);
    assert(!input.control); // The Home mascot is decorative, not an action.
    unsigned home_confirmations = confirmations;
    unsigned original_home_opens = home_opens;
    touch(178, 62, true); touch(178, 62, false);
    touch(90, 62, true); touch(90, 62, false);
    assert(home_menus == 1 && home_agents == 1 && home_opens == original_home_opens);
    touch(178, 62, true); touch(150, 96, true); touch(150, 96, false);
    touch(90, 62, true); touch(90, 100, true); touch(90, 100, false);
    assert(home_menus == 1 && home_agents == 1);
    touch(178, 62, true);
    dial_input_set_control(&input, false);
    touch(178, 62, false);
    touch(120, 201, true);
    dial_input_set_control(&input, false);
    touch(120, 201, false);
    assert(home_menus == 1 && home_opens == original_home_opens && !input.control);
    touch(120, 201, true); touch(120, 201, false);
    assert(home_opens == original_home_opens + 1 && input.control && confirmations == home_confirmations);
    // Hidden Home controls cannot dispatch a queued click over another view.
    lv_obj_send_event(home.menu_button, LV_EVENT_SHORT_CLICKED, NULL);
    assert(home_menus == 1);
    lv_obj_set_hidden(input.arc, true);
    lv_obj_set_hidden(home.root, false);
    dial_input_set_control(&input, false);
    press_encoder(encoder);
    assert(home_opens == original_home_opens + 2 && input.control && confirmations == home_confirmations);
    lv_obj_set_hidden(input.arc, true);
    lv_obj_set_hidden(home.root, false);
    dial_input_set_control(&input, false);
    unsigned home_returns = homes;
    touch(178, 62, true); advance(1000); touch(178, 62, false);
    assert(homes == home_returns + 1 && home_menus == 1 && !input.control);
    navigating = true;
    lv_obj_set_hidden(home.root, true);
    dial_navigation_open_home_connection(&navigation);
    advance(40);
    snapshot("home-connection");
    assert(navigation.view == DIAL_NAV_CONNECTION);
    touch(68, 44, true); touch(68, 44, false);
    assert(!navigating && lv_obj_is_visible(home.root));
    assert(confirmations == home_confirmations);
    const lv_point_t home_targets[] = {{178, 62}, {90, 62}, {120, 201}};
    for (unsigned i = 0; i < sizeof(home_targets) / sizeof(home_targets[0]); ++i) {
        touch(home_targets[i].x, home_targets[i].y, true);
        control();
        touch(home_targets[i].x, home_targets[i].y, false);
        assert(confirmations == home_confirmations);
        lv_obj_set_hidden(input.arc, true);
        lv_obj_set_hidden(home.root, false);
        dial_input_set_control(&input, false);
    }
    assert(home_menus == 1 && home_agents == 1 && home_opens == original_home_opens + 2);
    puts("Home targets: decorative center, distinct actions, gesture fences and Connection Back passed");
    assert(reader_boundary_failures == 0);
    lv_deinit();
}
