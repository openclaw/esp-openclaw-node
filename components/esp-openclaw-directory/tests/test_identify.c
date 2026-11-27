#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../esp_openclaw_devices.c"
#include "esp_openclaw_identify.h"

static int locked, queries;
static int64_t now;
static esp_err_t queue_result = ESP_OK;
static esp_openclaw_node_gateway_request_cb_t callback;
static void *callback_context;
static char params[1024];
static esp_openclaw_devices_snapshot_t page;
struct fake_timer { bool armed; };
static struct fake_timer timers[2];
static unsigned timer_count;
static esp_openclaw_node_handle_t client = (esp_openclaw_node_handle_t)&queries;

int64_t esp_timer_get_time(void) { return now; }
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out)
{ (void)args; assert(timer_count < 2); *out = &timers[timer_count++]; return ESP_OK; }
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t delay)
{ assert(timer && delay); if (timer->armed) return ESP_ERR_INVALID_STATE; timer->armed = true; return ESP_OK; }
esp_err_t esp_timer_stop(esp_timer_handle_t timer) { assert(timer); timer->armed = false; return ESP_OK; }
void esp_fill_random(void *buffer, size_t length) { memset(buffer, 0x42, length); }
void xSemaphoreTake(SemaphoreHandle_t lock, uint32_t timeout)
{ assert(lock == &locked && !locked && timeout == portMAX_DELAY); locked = 1; }
void xSemaphoreGive(SemaphoreHandle_t lock) { assert(lock == &locked && locked); locked = 0; }
static void wake(void) {}
static bool immediate_owner = true;
static bool authority_current(bool write) { (void)write; assert(locked); return immediate_owner; }

esp_err_t esp_openclaw_node_gateway_request_on_connection(esp_openclaw_node_handle_t handle,
    uint32_t connection, const char *method, const char *json,
    esp_openclaw_node_gateway_request_cb_t cb, void *context)
{
    assert(!locked && handle == client && connection == 7 && !callback);
    assert(strcmp(method, "node.invoke") == 0 && strlen(json) < sizeof(params));
    strcpy(params, json); ++queries;
    if (queue_result == ESP_OK) { callback = cb; callback_context = context; }
    return queue_result;
}

static esp_openclaw_identify_snapshot_t snapshot(void)
{
    esp_openclaw_identify_snapshot_t result;
    esp_openclaw_identify_copy(&result);
    return result;
}

static void owner(bool writable, uint64_t incarnation)
{
    xSemaphoreTake(&locked, portMAX_DELAY);
    esp_openclaw_identify_set_owner_locked(client, incarnation, 7, true, writable);
    xSemaphoreGive(&locked);
}

static void reply(bool rendered)
{
    assert(callback);
    char json[512];
    snprintf(json, sizeof(json), "{\"ok\":true,\"nodeId\":\"test-node\",\"command\":\"display.show\","
        "\"payload\":{\"ready\":true,\"heading\":\"Identify\",\"text\":\"Code: 4242424242424242\","
        "\"generation\":5,\"renderedGeneration\":%d,\"rendered\":%s}}", rendered ? 5 : 4, rendered ? "true" : "false");
    esp_openclaw_node_gateway_result_t result = {.ok = true, .payload_json = json};
    callback(client, &result, callback_context);
    callback = NULL; callback_context = NULL;
    esp_openclaw_identify_poll(false);
}

int main(void)
{
    assert(esp_openclaw_identify_init(&locked, wake, authority_current) == ESP_OK);
    directory.lock = &locked; directory.ready = directory.interested = true;
    directory.state = ESP_OPENCLAW_DEVICES_READY; directory.revision = 3; directory.page = &page;
    page.count = 1;
    esp_openclaw_device_t *device = &page.devices[0];
    strcpy(device->id, "test-node"); strcpy(device->name, "Board"); strcpy(device->model, "m5stack-m5stickc");
    device->connected = device->connected_known = device->paired = device->paired_known = true;
    device->commands_known = device->display_show = true;
    owner(false, 1);
    assert(esp_openclaw_identify_begin("test-node", 3) == ESP_OK);
    assert(snapshot().state == ESP_OPENCLAW_IDENTIFY_REVIEW && !snapshot().can_send);
    assert(esp_openclaw_identify_send(snapshot().revision) == ESP_ERR_INVALID_STATE);
    owner(true, 1);
    assert(snapshot().state == ESP_OPENCLAW_IDENTIFY_FAILED);
    assert(esp_openclaw_identify_begin("test-node", 2) == ESP_ERR_INVALID_STATE);
    assert(esp_openclaw_identify_begin("missing", 3) == ESP_ERR_INVALID_STATE);
    device->commands_known = false;
    assert(esp_openclaw_identify_begin("test-node", 3) == ESP_ERR_INVALID_STATE);
    device->commands_known = true; strcpy(device->model, "unknown-board");
    assert(esp_openclaw_identify_begin("test-node", 3) == ESP_ERR_INVALID_STATE);
    strcpy(device->model, "lilygo-t-embed");
    assert(esp_openclaw_identify_begin("test-node", 3) == ESP_OK);
    unsigned revision = snapshot().revision;
    assert(snapshot().can_send && queries == 0);
    assert(esp_openclaw_identify_send(revision - 1) == ESP_ERR_INVALID_STATE);
    assert(esp_openclaw_identify_send(revision) == ESP_OK);
    assert(esp_openclaw_identify_send(revision) == ESP_ERR_INVALID_STATE);
    esp_openclaw_identify_poll(false); assert(queries == 1);
    cJSON *root = cJSON_Parse(params); assert(root);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(root, "nodeId")->valuestring, "test-node") == 0);
    assert(cJSON_GetObjectItemCaseSensitive(root, "sessionKey") == NULL);
    assert(cJSON_GetObjectItemCaseSensitive(root, "idempotencyKey") != NULL);
    assert(cJSON_GetObjectItemCaseSensitive(root, "timeoutMs")->valueint == 10000);
    cJSON_Delete(root);
    assert(esp_openclaw_identify_begin("test-node", 3) == ESP_ERR_INVALID_STATE);
    reply(false); assert(snapshot().state == ESP_OPENCLAW_IDENTIFY_ACCEPTED);
    esp_openclaw_identify_poll(false); assert(queries == 1);

    assert(esp_openclaw_identify_begin("test-node", 3) == ESP_OK);
    assert(esp_openclaw_identify_send(snapshot().revision) == ESP_OK);
    esp_openclaw_identify_poll(false); reply(true);
    assert(snapshot().state == ESP_OPENCLAW_IDENTIFY_RENDERED);

    // Invalidate the reviewed inventory before admission: no command is sent.
    assert(esp_openclaw_identify_begin("test-node", 3) == ESP_OK);
    assert(esp_openclaw_identify_send(snapshot().revision) == ESP_OK);
    ++directory.revision;
    int before = queries;
    esp_openclaw_identify_poll(false);
    assert(snapshot().state == ESP_OPENCLAW_IDENTIFY_FAILED && queries == before);

    // A timed-out submission retains its callback slot and never replays.
    assert(esp_openclaw_identify_begin("test-node", 4) == ESP_OK);
    assert(esp_openclaw_identify_send(snapshot().revision) == ESP_OK);
    esp_openclaw_identify_poll(false); before = queries;
    now += 15000000; esp_openclaw_identify_poll(false);
    assert(snapshot().state == ESP_OPENCLAW_IDENTIFY_UNKNOWN);
    assert(esp_openclaw_identify_begin("test-node", 4) == ESP_ERR_INVALID_STATE);
    reply(true); assert(snapshot().state == ESP_OPENCLAW_IDENTIFY_UNKNOWN && queries == before);

    // A callback that reaches the worker after its deadline is also unknown,
    // even when the timer has not been polled yet.
    assert(esp_openclaw_identify_begin("test-node", 4) == ESP_OK);
    assert(esp_openclaw_identify_send(snapshot().revision) == ESP_OK);
    esp_openclaw_identify_poll(false); now += 15000000;
    reply(true); assert(snapshot().state == ESP_OPENCLAW_IDENTIFY_UNKNOWN);

    assert(esp_openclaw_identify_begin("test-node", 4) == ESP_OK);
    assert(esp_openclaw_identify_send(snapshot().revision) == ESP_OK);
    esp_openclaw_identify_poll(false); immediate_owner = false;
    reply(true); assert(snapshot().state == ESP_OPENCLAW_IDENTIFY_UNKNOWN);
    immediate_owner = true;

    assert(esp_openclaw_identify_begin("test-node", 4) == ESP_OK);
    assert(esp_openclaw_identify_send(snapshot().revision) == ESP_OK);
    esp_openclaw_identify_poll(false); owner(true, 2);
    reply(true); assert(snapshot().state == ESP_OPENCLAW_IDENTIFY_UNKNOWN);

    // Immediate owner invalidation is checked before the worker updates metadata.
    assert(esp_openclaw_identify_begin("test-node", 4) == ESP_OK);
    immediate_owner = false;
    assert(!snapshot().can_send);
    assert(esp_openclaw_identify_send(snapshot().revision) == ESP_ERR_INVALID_STATE);
    immediate_owner = true;

    // Rejected local queue admission has a known no-send outcome.
    queue_result = ESP_ERR_NO_MEM;
    assert(esp_openclaw_identify_begin("test-node", 4) == ESP_OK);
    assert(esp_openclaw_identify_send(snapshot().revision) == ESP_OK);
    esp_openclaw_identify_poll(false); esp_openclaw_identify_poll(false);
    assert(snapshot().state == ESP_OPENCLAW_IDENTIFY_FAILED);
    queue_result = ESP_OK;
    assert(esp_openclaw_identify_begin("test-node", 4) == ESP_OK);
    assert(esp_openclaw_identify_send(snapshot().revision) == ESP_OK);
    esp_openclaw_identify_poll(false); reply(true);
    assert(snapshot().state == ESP_OPENCLAW_IDENTIFY_RENDERED);
    puts("Identify: authority, target, revision, exact-once admission, receipt and loss checks passed");
    return 0;
}
