// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compact_workspace.h"
#include "esp_openclaw_reader.h"
#include "lvgl.h"
#include "openclaw_preferences.h"

static esp_openclaw_reader_agents_t agents;
static esp_openclaw_reader_conversations_t conversations;
static esp_openclaw_reader_reply_snapshot_t reply;
static esp_openclaw_reader_creation_snapshot_t creation;
static esp_openclaw_reader_messaging_snapshot_t messaging;
static esp_openclaw_devices_snapshot_t devices;
static esp_openclaw_identify_snapshot_t identify;
static unsigned identifies;
static char identify_target[129];
static esp_openclaw_reader_home_t home_context;
static unsigned refreshes, opens, previews, sends;
static unsigned text_drafts;
static uint32_t now;
static int width, height, brightness;
static uint16_t frame[320 * 170], buffer[320 * 20];
static esp_openclaw_node_wifi_status_t wifi = {.connected = true, .ssid = "Test network", .ip = "192.0.2.10"};
static const char *snapshot_prefix;
static bool reader_connected = true, node_connected = true;
static bool reject_local_send;

const char *esp_err_to_name(esp_err_t err) { return err == ESP_OK ? "OK" : "error"; }
int openclaw_preferences_load(openclaw_preference_t key, int fallback) { (void)key; return fallback; }
esp_err_t openclaw_preferences_save(openclaw_preference_t key, int value) { (void)key; (void)value; return ESP_OK; }
cJSON *esp_openclaw_reader_status(void) {
    cJSON *value = cJSON_CreateObject(); cJSON_AddBoolToObject(value, "readConnected", reader_connected); return value;
}
void esp_openclaw_reader_agents_copy(esp_openclaw_reader_agents_t *value) { *value = agents; }
void esp_openclaw_reader_conversations_copy(esp_openclaw_reader_conversations_t *value) { *value = conversations; }
void esp_openclaw_reader_reply_copy(esp_openclaw_reader_reply_snapshot_t *value) { *value = reply; }
void esp_openclaw_reader_creation_copy(esp_openclaw_reader_creation_snapshot_t *value) { *value = creation; }
void esp_openclaw_reader_messaging_copy(esp_openclaw_reader_messaging_snapshot_t *value) { *value = messaging; }
void esp_openclaw_reader_devices_copy(esp_openclaw_devices_snapshot_t *value) { *value = devices; }
void esp_openclaw_reader_identify_copy(esp_openclaw_identify_snapshot_t *value) { *value = identify; }
esp_err_t esp_openclaw_reader_identify_begin(const char *id, unsigned revision) {
    assert(revision == devices.revision);
    size_t index = 0;
    while (index < devices.count && strcmp(id, devices.devices[index].id) != 0) ++index;
    assert(index < devices.count); strcpy(identify_target, id);
    identify.target = devices.devices[index]; identify.state = ESP_OPENCLAW_IDENTIFY_REVIEW;
    identify.can_send = true; strcpy(identify.text, "Code: 0123456789abcdef"); ++identify.revision;
    return ESP_OK;
}
esp_err_t esp_openclaw_reader_identify_send(unsigned revision) {
    assert(identify.can_send && revision == identify.revision); ++identifies;
    identify.state = ESP_OPENCLAW_IDENTIFY_RENDERED; identify.can_send = false; ++identify.revision;
    return ESP_OK;
}
void esp_openclaw_reader_home_copy(esp_openclaw_reader_home_t *value) { *value = home_context; }
esp_err_t esp_openclaw_reader_agents_refresh(size_t offset) { assert(offset == 0); ++refreshes; return ESP_OK; }
esp_err_t esp_openclaw_reader_conversations_open(const char *id, unsigned revision) {
    assert(strcmp(id, "assistant") == 0 && revision == agents.revision); ++opens; return ESP_OK;
}
esp_err_t esp_openclaw_reader_preview_open(size_t index, unsigned revision) {
    assert(index == 0 && revision == conversations.revision); ++previews; return ESP_OK;
}
esp_err_t esp_openclaw_reader_conversations_refresh(size_t offset) { (void)offset; return ESP_OK; }
esp_err_t esp_openclaw_reader_devices_refresh(void) { return ESP_OK; }
esp_err_t esp_openclaw_reader_reply_begin(esp_openclaw_reader_reply_preset_t preset, unsigned revision) {
    assert(preset == ESP_OPENCLAW_READER_REPLY_CONTINUE && revision == conversations.preview_revision);
    reply.state = ESP_OPENCLAW_READER_REPLY_REVIEW; reply.can_send = true;
    strcpy(reply.text, "Please continue."); ++reply.revision; return ESP_OK;
}
esp_err_t esp_openclaw_reader_reply_send(unsigned revision) {
    assert(revision == reply.revision && reply.can_send); ++sends;
    if (reject_local_send) {
        reply.state = ESP_OPENCLAW_READER_REPLY_REVIEW;
        strcpy(reply.detail, "Could not queue. Try Send again."); ++reply.revision;
        return ESP_OK;
    }
    reply.can_send = false; reply.state = ESP_OPENCLAW_READER_REPLY_UNKNOWN;
    strcpy(reply.detail, "Outcome unknown. Read before retrying."); ++reply.revision; return ESP_OK;
}
esp_err_t esp_openclaw_reader_reply_begin_text(const char *text, size_t length, unsigned revision) {
    assert(revision == conversations.preview_revision && length > 0 && length < sizeof(reply.text));
    ++text_drafts; memcpy(reply.text, text, length); reply.text[length] = '\0';
    reply.state = ESP_OPENCLAW_READER_REPLY_REVIEW; reply.can_send = true;
    reply.preview_revision = revision;
    reply.agent = conversations.agent;
    strcpy(reply.title, conversations.page.sessions[0].title); ++reply.revision;
    return ESP_OK;
}
void esp_openclaw_reader_reply_close(unsigned revision) {
    assert(revision == reply.revision);
    reply.state = ESP_OPENCLAW_READER_REPLY_IDLE; reply.can_send = false; ++reply.revision;
}
esp_err_t esp_openclaw_reader_reply_read(unsigned revision) { (void)revision; return ESP_OK; }
esp_err_t esp_openclaw_reader_creation_begin(unsigned revision) { (void)revision; return ESP_OK; }
esp_err_t esp_openclaw_reader_creation_submit(unsigned revision) { (void)revision; return ESP_OK; }
esp_err_t esp_openclaw_reader_creation_open(unsigned revision) { (void)revision; return ESP_OK; }
esp_err_t esp_openclaw_reader_messaging_request(unsigned revision) { (void)revision; return ESP_OK; }
esp_err_t esp_openclaw_reader_messaging_cancel(unsigned revision) { (void)revision; return ESP_OK; }

static esp_err_t backlight(int percent) { assert(percent >= 10 && percent <= 100); brightness = percent; return ESP_OK; }
static uint32_t tick(void) { return now; }
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *data) {
    int stride = lv_draw_buf_width_to_stride(area->x2 - area->x1 + 1, LV_COLOR_FORMAT_RGB565) / 2;
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x)
            frame[y * width + x] = ((uint16_t *)data)[(y - area->y1) * stride + x - area->x1];
    lv_display_flush_ready(display);
}
static void step(void) {
    now += 300; compact_workspace_poll(&wifi, node_connected, now); lv_timer_handler(); lv_refr_now(NULL);
}
static void select_item(void) { compact_workspace_input((compact_input_event_t){.select = true}); step(); }
static void move(int count) { compact_workspace_input((compact_input_event_t){.move = count}); step(); }
static void home(void) { compact_workspace_input((compact_input_event_t){.home = true}); step(); }
static bool find_row(const char *name) {
    char first[128]; snprintf(first, sizeof(first), "%s", compact_workspace_selection());
    for (unsigned i = 0; i < 25; ++i) {
        if (strcmp(compact_workspace_selection(), name) == 0) return true;
        move(1);
        if (strcmp(compact_workspace_selection(), first) == 0) return false;
    }
    return false;
}
static void choose(const char *name) { assert(find_row(name)); select_item(); }
static void snapshot(const char *name) {
    if (!snapshot_prefix) return;
    char path[1024]; assert(snprintf(path, sizeof(path), "%s-%s.ppm", snapshot_prefix, name) < (int)sizeof(path));
    FILE *file = fopen(path, "wb"); assert(file);
    fprintf(file, "P6\n%d %d\n255\n", width, height);
    for (int i = 0; i < width * height; ++i) {
        unsigned p = frame[i]; unsigned char rgb[] = {(p >> 11) * 255 / 31, ((p >> 5) & 63) * 255 / 63, (p & 31) * 255 / 31};
        assert(fwrite(rgb, 1, 3, file) == 3);
    }
    assert(fclose(file) == 0);
}

int main(int argc, char **argv) {
    assert(argc >= 2); width = atoi(argv[1]); height = width == 160 ? 80 : 170;
    snapshot_prefix = argc > 2 ? argv[2] : NULL;
    agents.state = ESP_OPENCLAW_READER_READY; agents.revision = 1; agents.page.count = agents.page.total = 1;
    strcpy(agents.page.agents[0].id, "assistant"); strcpy(agents.page.agents[0].name, "Assistant");
    conversations.state = conversations.preview_state = ESP_OPENCLAW_READER_READY;
    conversations.revision = conversations.preview_revision = 1;
    conversations.agent = agents.page.agents[0]; conversations.page.count = 1;
    strcpy(conversations.page.sessions[0].key, "agent:assistant:thread:one");
    strcpy(conversations.page.sessions[0].title, "Planning a good day");
    conversations.preview.count = 1; conversations.preview.status = ESP_OPENCLAW_PREVIEW_OK;
    conversations.preview.items[0].role = ESP_OPENCLAW_PREVIEW_ASSISTANT;
    strcpy(conversations.preview.items[0].text, "What would you like to work on? We can choose the next useful step together.");
    messaging.state = ESP_OPENCLAW_READER_MESSAGING_READY;
    lv_init(); lv_tick_set_cb(tick);
    lv_display_t *display = lv_display_create(width, height);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buffer, NULL, width * 20 * sizeof(uint16_t), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    compact_workspace_start(width, height, width == 320, backlight); step();
    assert(brightness == 80); assert(strcmp(compact_workspace_view(), "home") == 0); snapshot("home");
    unsigned revision = compact_workspace_revision();
    reader_connected = false; step(); assert(compact_workspace_revision() != revision);
    revision = compact_workspace_revision();
    reader_connected = true; step(); assert(compact_workspace_revision() != revision);
    revision = compact_workspace_revision();
    node_connected = false; step(); assert(compact_workspace_revision() != revision);
    node_connected = true; step();
    reader_connected = false; step(); select_item();
    assert(strcmp(compact_workspace_view(), "connection") == 0);
    revision = compact_workspace_revision();
    reader_connected = true; step(); assert(compact_workspace_revision() != revision);
    select_item(); assert(strcmp(compact_workspace_view(), "home") == 0);
    move(1); assert(strcmp(compact_workspace_view(), "menu") == 0);
    assert(strcmp(compact_workspace_selection(), "Agents") == 0);
    compact_workspace_input((compact_input_event_t){.move = 1, .select = true}); step();
    assert(strcmp(compact_workspace_view(), "menu") == 0 && opens == 0);
    assert(strcmp(compact_workspace_selection(), "Conversations") == 0);
    move(2); select_item();
    assert(strcmp(compact_workspace_view(), "activity") == 0 && !compact_workspace_message_visible());
    home(); move(1); move(2); select_item();
    assert(strcmp(compact_workspace_view(), "devices") == 0);
    select_item(); assert(strcmp(compact_workspace_view(), "local-device") == 0);
    select_item(); assert(strcmp(compact_workspace_view(), "display") == 0);
    move(4); select_item(); assert(strcmp(compact_workspace_view(), "local-device") == 0);
    move(3); select_item(); assert(strcmp(compact_workspace_view(), "devices") == 0);
    home();
    select_item(); assert(strcmp(compact_workspace_view(), "agents") == 0 && refreshes == 1); snapshot("agents");
    select_item(); assert(strcmp(compact_workspace_view(), "conversations") == 0 && opens == 1);
    agents.page.offset = 8;
    move(4); select_item(); assert(strcmp(compact_workspace_view(), "agents") == 0 && refreshes == 1);
    select_item(); assert(strcmp(compact_workspace_view(), "conversations") == 0 && opens == 2);
    home(); move(1); move(1); select_item();
    assert(strcmp(compact_workspace_view(), "conversations") == 0);
    move(4); select_item(); assert(strcmp(compact_workspace_view(), "menu") == 0);
    select_item(); assert(strcmp(compact_workspace_view(), "conversations") == 0);
    select_item(); assert(strcmp(compact_workspace_view(), "reader") == 0 && previews == 1); snapshot("reader");
    select_item(); assert(strcmp(compact_workspace_view(), "reader-actions") == 0);
    select_item(); assert(strcmp(compact_workspace_view(), "reply-picker") == 0);
    select_item(); assert(strcmp(compact_workspace_view(), "reply-review") == 0 && sends == 0);
    select_item(); assert(sends == 1);
    compact_workspace_message("From Gateway", "A real incoming message", true, 1); step();
    assert(strcmp(compact_workspace_view(), "reply-review") == 0 && !compact_workspace_message_visible());
    select_item(); assert(sends == 1);
    home(); move(1); move(3); select_item();
    assert(compact_workspace_message_visible());
    assert(compact_workspace_message_line_height() == (width == 160 ? lv_font_montserrat_20.line_height : lv_font_montserrat_28.line_height));
    home();
    assert(!compact_workspace_message_visible());
    revision = compact_workspace_revision();
    home_context.available = true; step(); assert(compact_workspace_revision() != revision);
    revision = compact_workspace_revision();
    home_context.preview = true; step(); assert(compact_workspace_revision() != revision);
    select_item(); assert(strcmp(compact_workspace_view(), "reader") == 0);
    select_item(); move(3); select_item(); assert(strcmp(compact_workspace_view(), "conversations") == 0);
    move(4); select_item(); assert(strcmp(compact_workspace_view(), "home") == 0);
    devices.state = ESP_OPENCLAW_DEVICES_READY; devices.revision = 1; devices.count = 1;
    strcpy(devices.devices[0].id, "test-node"); strcpy(devices.devices[0].name, "Display board");
    strcpy(devices.devices[0].model, "m5stack-m5stickc");
    devices.devices[0].connected = devices.devices[0].connected_known = true;
    devices.devices[0].paired = devices.devices[0].paired_known = true;
    devices.devices[0].commands_known = devices.devices[0].display_show = true;
    home(); move(1); move(2); select_item(); move(1); select_item();
    assert(strcmp(compact_workspace_view(), "device") == 0); select_item();
    assert(strcmp(compact_workspace_view(), "device-actions") == 0);
    assert(strcmp(compact_workspace_selection(), "Identify") == 0); select_item();
    assert(strcmp(compact_workspace_view(), "identify") == 0 && identifies == 0); snapshot("identify-review");
    select_item(); assert(strcmp(compact_workspace_view(), "identify-confirm") == 0 && identifies == 0);
    move(2); select_item(); assert(strcmp(compact_workspace_view(), "device-actions") == 0 && identifies == 0);
    select_item(); select_item(); select_item();
    assert(strcmp(compact_workspace_view(), "identify") == 0 && identifies == 1); snapshot("identify-rendered");
    // Reviewing and cancelling B preserves the submitted result and owner A.
    devices.devices[1] = devices.devices[0]; devices.count = 2; ++devices.revision;
    strcpy(devices.devices[1].id, "second-node"); strcpy(devices.devices[1].name, "Other board");
    home(); move(1); move(2); select_item(); move(2); select_item(); select_item(); select_item();
    assert(strcmp(identify_target, "second-node") == 0 && identifies == 1);
    select_item(); move(2); select_item();
    home(); move(1); move(3); select_item(); select_item(); select_item();
    assert(strcmp(compact_workspace_view(), "identify") == 0 && identifies == 1);
    select_item(); select_item();
    assert(strcmp(identify_target, "test-node") == 0 && identifies == 1);
    devices.count = 0; ++devices.revision; step(); select_item();
    home(); move(1); move(2); select_item();
    assert(strcmp(compact_workspace_view(), "devices") == 0 && identifies == 1);
    home(); move(1); move(3); select_item(); select_item();
    assert(strcmp(compact_workspace_view(), "activity-actions") == 0);
    select_item(); assert(strcmp(compact_workspace_view(), "identify") == 0 && identifies == 1);
    select_item(); assert(strcmp(compact_workspace_view(), "devices") == 0 && identifies == 1);
    // A request revision identifies its owner, not completion. The same
    // revision must repaint when an asynchronous result becomes available.
    home(); move(1); select_item();
    agents.state = ESP_OPENCLAW_READER_LOADING; agents.page.count = agents.page.offset = 0; ++agents.revision; step();
    revision = compact_workspace_revision();
    compact_input_t held = {0};
    compact_input_sample(&held, 0, revision, false, false, width == 320, 3);
    compact_input_sample(&held, 30, revision, false, false, width == 320, 3);
    compact_input_sample(&held, 40, revision, true, false, width == 320, 3);
    compact_input_sample(&held, 70, revision, true, false, width == 320, 3);
    agents.state = ESP_OPENCLAW_READER_READY; agents.page.count = agents.page.total = 1; step();
    assert(compact_workspace_revision() != revision);
    assert(strcmp(compact_workspace_selection(), "Assistant") == 0);
    compact_input_sample(&held, 100, compact_workspace_revision(), false, false, width == 320, 3);
    compact_input_event_t release = compact_input_sample(&held, 130, compact_workspace_revision(), false, false, width == 320, 3);
    unsigned before_opens = opens;
    compact_workspace_input(release); step();
    assert(!release.select && opens == before_opens && strcmp(compact_workspace_view(), "agents") == 0);
    revision = compact_workspace_revision(); step(); assert(compact_workspace_revision() == revision);
    agents.state = ESP_OPENCLAW_READER_EMPTY; agents.page.count = agents.page.total = 0; step();
    assert(compact_workspace_revision() != revision && strcmp(compact_workspace_selection(), "Refresh") == 0);
    revision = compact_workspace_revision(); agents.state = ESP_OPENCLAW_READER_ERROR; step();
    assert(compact_workspace_revision() != revision);
    agents.state = ESP_OPENCLAW_READER_READY; agents.page.count = agents.page.total = 1; step();
    select_item();
    conversations.state = ESP_OPENCLAW_READER_LOADING; conversations.page.count = 0; ++conversations.revision; step();
    revision = compact_workspace_revision();
    conversations.state = ESP_OPENCLAW_READER_READY; conversations.page.count = 1; step();
    assert(compact_workspace_revision() != revision);
    assert(strcmp(compact_workspace_selection(), "Planning a good day") == 0);
    select_item();
    conversations.preview_state = ESP_OPENCLAW_READER_LOADING; ++conversations.preview_revision; step();
    revision = compact_workspace_revision();
    conversations.preview_state = ESP_OPENCLAW_READER_READY; step();
    assert(compact_workspace_revision() != revision);
    home(); move(1); move(2); select_item();
    devices.state = ESP_OPENCLAW_DEVICES_LOADING; devices.count = 0; ++devices.revision; step();
    revision = compact_workspace_revision();
    devices.state = ESP_OPENCLAW_DEVICES_READY; devices.count = 1; step();
    assert(compact_workspace_revision() != revision);
    home(); move(1); choose("Conversations"); choose("Planning a good day"); select_item(); choose("Reply"); choose("Write message");
    assert(strcmp(compact_workspace_view(), "draft") == 0);
    choose("A-M"); choose("H"); choose("Back"); choose("a-m"); choose("i"); choose("Back");
    choose("Preview"); assert(strcmp(compact_workspace_view(), "draft-preview") == 0); snapshot("draft-preview");
    select_item(); choose("Review"); assert(strcmp(reply.text, "Hi") == 0 && text_drafts == 1);
    select_item(); assert(strcmp(compact_workspace_view(), "reply-confirm") == 0);
    unsigned sends_before = sends; choose("Edit"); assert(sends == sends_before);
    choose("Space"); choose("N-Z"); choose("O"); choose("Back"); choose("A-M"); choose("K"); choose("Back");
    choose("Review"); assert(strcmp(reply.text, "Hi OK") == 0 && sends == sends_before);
    select_item(); choose("Cancel"); assert(sends == sends_before);
    select_item(); choose("Reply"); choose("Write message"); choose("Resume draft"); choose("Review");
    select_item(); reject_local_send = true; choose("Send");
    assert(sends == sends_before + 1 && strcmp(reply.text, "Hi OK") == 0);
    select_item(); choose("Edit"); choose("Review");
    assert(strcmp(reply.text, "Hi OK") == 0 && sends == sends_before + 1);
    select_item(); reject_local_send = false; choose("Send");
    assert(sends == sends_before + 2 && strcmp(reply.text, "Hi OK") == 0);
    select_item(); assert(sends == sends_before + 2);
    home(); move(1); choose("Conversations"); choose("Planning a good day"); select_item(); choose("Reply"); choose("Write message");
    choose("a-m");
    for (unsigned i = 0; i < 160; ++i) select_item();
    choose("Back"); choose("Review"); assert(strlen(reply.text) == 159);
    select_item(); choose("Cancel"); select_item(); choose("Reply"); choose("Write message"); choose("Resume draft");
    ++conversations.preview_revision; step();
    assert(!find_row("Review"));
    choose("Back to messages"); select_item(); choose("Reply"); choose("Write message");
    assert(!find_row("Resume draft")); choose("Discard draft"); choose("Write message");
    unsigned drafts_before = text_drafts; choose("Review"); assert(text_drafts == drafts_before);
    choose("a-m"); choose("a"); choose("Back"); choose("Preview");
    reader_connected = false; step(); select_item();
    assert(strcmp(compact_workspace_view(), "draft-choice") == 0);
    choose("Discard draft"); assert(strcmp(compact_workspace_view(), "reply-picker") == 0);
    assert(text_drafts == drafts_before);
    reader_connected = true; step(); choose("Write message");
    assert(strcmp(compact_workspace_view(), "draft") == 0);
    printf("Compact workspace %dx%d: async results, owned drafts, reviewed actions and retained outcomes passed\n", width, height);
    lv_deinit();
}
