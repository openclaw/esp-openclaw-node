#include "wifi_host_sdk.h"
#include "esp_openclaw_node_wifi.h"
#include "esp_openclaw_node_common_device_node_cmd.h"
#include "cJSON.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

const char *WIFI_EVENT = "wifi", *IP_EVENT = "ip";
static esp_event_handler_t ip_handler;
static void *ip_context;
static EventBits_t event_bits;
static esp_openclaw_node_command_t wifi_command;
static wifi_ap_record_t ap;
static esp_err_t ap_result;
static unsigned ap_reads, failures;

const char *esp_err_to_name(esp_err_t error) { (void)error; return "test error"; }
EventGroupHandle_t xEventGroupCreate(void) { return &event_bits; }
EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits) { (void)group; event_bits &= ~bits; return event_bits; }
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits) { (void)group; event_bits |= bits; return event_bits; }
EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits, int clear, int all, TickType_t timeout)
{ (void)group; (void)bits; (void)clear; (void)all; (void)timeout; return event_bits; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { static int lock; return &lock; }
int xSemaphoreTake(SemaphoreHandle_t lock, TickType_t timeout) { assert(lock); (void)timeout; return 1; }
int xSemaphoreGive(SemaphoreHandle_t lock) { assert(lock); return 1; }
esp_err_t esp_event_handler_register(esp_event_base_t base, int32_t id, esp_event_handler_t handler, void *context)
{
    if (base == IP_EVENT) { assert(id == IP_EVENT_STA_GOT_IP); ip_handler = handler; ip_context = context; }
    return ESP_OK;
}
void *esp_netif_create_default_wifi_sta(void) { static int netif; return &netif; }
esp_err_t esp_wifi_init(const wifi_init_config_t *config) { (void)config; return ESP_OK; }
esp_err_t esp_wifi_set_storage(int storage) { (void)storage; return ESP_OK; }
esp_err_t esp_wifi_set_mode(int mode) { (void)mode; return ESP_OK; }
esp_err_t esp_wifi_get_config(int iface, wifi_config_t *config)
{ (void)iface; *config = (wifi_config_t){0}; memcpy(config->sta.ssid, "Example Wi-Fi", 13); return ESP_OK; }
esp_err_t esp_wifi_set_config(int iface, const wifi_config_t *config) { (void)iface; (void)config; return ESP_OK; }
esp_err_t esp_wifi_start(void) { return ESP_OK; }
esp_err_t esp_wifi_stop(void) { return ESP_OK; }
esp_err_t esp_wifi_restore(void) { return ESP_OK; }
esp_err_t esp_wifi_set_ps(int mode) { (void)mode; return ESP_OK; }
esp_err_t esp_wifi_connect(void) { return ESP_OK; }
esp_err_t esp_wifi_disconnect(void) { return ESP_OK; }
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *record) { ++ap_reads; *record = ap; return ap_result; }
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config, bool block) { (void)config; (void)block; return ESP_OK; }
esp_err_t esp_wifi_scan_get_ap_num(uint16_t *count) { *count = 0; return ESP_OK; }
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *count, wifi_ap_record_t *records) { (void)records; *count = 0; return ESP_OK; }
esp_err_t esp_wifi_clear_ap_list(void) { return ESP_OK; }
esp_err_t esp_openclaw_node_register_capability(esp_openclaw_node_handle_t node, const char *name)
{ (void)node; (void)name; return ESP_OK; }
esp_err_t esp_openclaw_node_register_command(esp_openclaw_node_handle_t node, const esp_openclaw_node_command_t *command)
{ (void)node; if (strcmp(command->name, "wifi.status") == 0) wifi_command = *command; return ESP_OK; }
const esp_app_desc_t *esp_app_get_description(void) { static const esp_app_desc_t app = {.version = "test"}; return &app; }
const char *esp_get_idf_version(void) { return "host"; }
int64_t esp_timer_get_time(void) { return 1000000; }

static void check(bool condition, const char *message)
{
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

static cJSON *status(void)
{
    char *payload = NULL;
    esp_openclaw_node_error_t error = {0};
    assert(wifi_command.handler != NULL);
    assert(wifi_command.handler(NULL, wifi_command.context, "{}", 2, &payload, &error) == ESP_OK);
    assert(payload != NULL);
    cJSON *result = cJSON_Parse(payload);
    free(payload);
    assert(cJSON_IsObject(result));
    return result;
}

static void check_unknown(void)
{
    cJSON *result = status();
    check(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "connected")), "connection survives an unavailable AP observation");
    check(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(result, "rssi")), "unavailable RSSI is null, not zero");
    check(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(result, "channel")), "unavailable channel is null, not zero");
    const cJSON *auth = cJSON_GetObjectItemCaseSensitive(result, "authMode");
    check(cJSON_IsString(auth) && strcmp(auth->valuestring, "unknown") == 0, "unavailable security is unknown, not open");
    cJSON_Delete(result);
}

int main(void)
{
    assert(esp_openclaw_node_register_device_commands(NULL, NULL) == ESP_OK);
    cJSON *result = status();
    check(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(result, "connected")), "uninitialized Wi-Fi is disconnected");
    check(!cJSON_HasObjectItem(result, "rssi"), "disconnected status omits AP observations");
    cJSON_Delete(result);

    assert(esp_openclaw_node_wifi_start() == ESP_OK);
    ip_event_got_ip_t event = {.ip_info.ip.addr = 0x010200c0};
    assert(ip_handler != NULL);
    ip_handler(ip_context, IP_EVENT, IP_EVENT_STA_GOT_IP, &event);
    ap_result = ESP_FAIL;
    check_unknown();

    ap_result = ESP_OK;
    ap = (wifi_ap_record_t){.rssi = -63, .primary = 6, .authmode = WIFI_AUTH_WPA2_PSK};
    result = status();
    const cJSON *rssi = cJSON_GetObjectItemCaseSensitive(result, "rssi");
    const cJSON *channel = cJSON_GetObjectItemCaseSensitive(result, "channel");
    const cJSON *auth = cJSON_GetObjectItemCaseSensitive(result, "authMode");
    check(cJSON_IsNumber(rssi) && rssi->valueint == -63, "successful RSSI is reported");
    check(cJSON_IsNumber(channel) && channel->valueint == 6, "successful channel is reported");
    check(cJSON_IsString(auth) && strcmp(auth->valuestring, "wpa2-psk") == 0, "successful AP security is reported");
    cJSON_Delete(result);

    ap_result = ESP_ERR_WIFI_NOT_CONNECT;
    check_unknown();
    ap_result = ESP_OK;
    ap.rssi = 0;
    result = status();
    rssi = cJSON_GetObjectItemCaseSensitive(result, "rssi");
    check(cJSON_IsNumber(rssi) && rssi->valueint == 0, "validity follows the driver result, not an RSSI sentinel");
    cJSON_Delete(result);

    assert(esp_openclaw_node_wifi_disconnect() == ESP_OK);
    unsigned before = ap_reads;
    result = status();
    check(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(result, "connected")), "disconnect clears connection status");
    check(!cJSON_HasObjectItem(result, "rssi") && ap_reads == before, "disconnect does not publish or request AP observations");
    cJSON_Delete(result);
    printf("Wi-Fi status: %u failed checks\n", failures);
    return failures ? 1 : 0;
}
