#include "room_canvas.h"

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "room_canvas_internal.h"

#define TAG "room_canvas_surface"
#define ROOM_CANVAS_SURFACE_REFRESH_US (8LL * 60 * 1000000)

static void surface_refresh_response(
    esp_openclaw_node_handle_t responding_client,
    const esp_openclaw_node_gateway_result_t *result,
    void *user_ctx)
{
    (void)responding_client;
    (void)user_ctx;
    if (!result->ok || result->payload_json == NULL) {
        ESP_LOGW(
            TAG,
            "canvas surface refresh failed: %s",
            result->error_message != NULL ? result->error_message : "unknown");
        return;
    }
    cJSON *payload = cJSON_Parse(result->payload_json);
    cJSON *urls = cJSON_IsObject(payload)
        ? cJSON_GetObjectItemCaseSensitive(payload, "pluginSurfaceUrls")
        : NULL;
    cJSON *url = cJSON_IsObject(urls)
        ? cJSON_GetObjectItemCaseSensitive(urls, "canvas")
        : NULL;
    /* Store on the NODE client: dup_plugin_surface_url there stays the single
     * read path for canvas URL resolution regardless of which role minted. */
    if (cJSON_IsString(url) && url->valuestring != NULL && room_canvas_node != NULL &&
        esp_openclaw_node_store_plugin_surface_url(room_canvas_node, "canvas", url->valuestring) == ESP_OK) {
        ESP_LOGI(TAG, "canvas surface URL refreshed");
    } else {
        ESP_LOGW(TAG, "canvas surface refresh returned no canvas URL");
    }
    cJSON_Delete(payload);
}

static esp_openclaw_node_handle_t refresh_client;

static void surface_refresh_tick(void *arg)
{
    (void)arg;
    if (refresh_client == NULL) {
        return;
    }
    /* plugin.surface.refresh needs operator scope, so it rides the operator
     * connection; the token it mints authorizes because the gateway accepts a
     * capability from any live client that owns it. Away from the gateway the
     * request fails UNAVAILABLE and the next tick retries; a reconnect mints
     * fresh URLs on its own via hello-ok. */
    (void)esp_openclaw_node_gateway_request(
        refresh_client,
        "plugin.surface.refresh",
        "{\"surface\":\"canvas\"}",
        surface_refresh_response,
        NULL);
}

void room_canvas_set_node(esp_openclaw_node_handle_t node)
{
    room_canvas_node = node;
}

void room_canvas_set_refresh_client(esp_openclaw_node_handle_t operator_node)
{
    refresh_client = operator_node;
    static esp_timer_handle_t refresh_timer;
    if (refresh_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = surface_refresh_tick,
            .name = "canvas_surface",
        };
        if (esp_timer_create(&args, &refresh_timer) == ESP_OK &&
            esp_timer_start_periodic(refresh_timer, ROOM_CANVAS_SURFACE_REFRESH_US) != ESP_OK) {
            ESP_LOGW(TAG, "canvas surface keep-warm timer unavailable");
        }
    }
}

