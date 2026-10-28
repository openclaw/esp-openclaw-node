#include "wifi_host_sdk.h"
#include "esp_openclaw_node_wifi.h"
#include "esp_openclaw_node_common_device_node_cmd.h"
#include "cJSON.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

const char *WIFI_EVENT = "wifi", *IP_EVENT = "ip";
static esp_event_handler_t ip_handler, wifi_handler;
static void *ip_context, *wifi_context;
static EventBits_t event_bits;
static esp_openclaw_node_command_t wifi_command;
static wifi_ap_record_t ap;
static esp_err_t ap_result;
static unsigned ap_reads, failures;
static unsigned driver_mutations, connects;
static bool probe_scan, inject_disconnect;
static esp_err_t config_result = ESP_OK, scan_result = ESP_OK;
static esp_err_t connect_result = ESP_OK;
static bool probe_reconnect_unlock;
static esp_openclaw_node_wifi_setup_lease_t scan_lease;

static void check(bool condition, const char *message);

const char *esp_err_to_name(esp_err_t error) { (void)error; return "test error"; }
EventGroupHandle_t xEventGroupCreate(void) { return &event_bits; }
EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits) { (void)group; event_bits &= ~bits; return event_bits; }
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits) { (void)group; event_bits |= bits; return event_bits; }
EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits, int clear, int all, TickType_t timeout)
{ (void)group; (void)bits; (void)clear; (void)all; (void)timeout; return event_bits; }
SemaphoreHandle_t xSemaphoreCreateMutex(void)
{ static bool locks[4]; static unsigned count; assert(count < 4); return &locks[count++]; }
int xSemaphoreTake(SemaphoreHandle_t lock, TickType_t timeout)
{ assert(lock && !*(bool *)lock); (void)timeout; *(bool *)lock = true; return 1; }
int xSemaphoreGive(SemaphoreHandle_t lock)
{
    assert(lock && *(bool *)lock); *(bool *)lock = false;
    if (probe_reconnect_unlock) {
        probe_reconnect_unlock = false;
        esp_openclaw_node_wifi_setup_lease_t lease = 0;
        check(esp_openclaw_node_wifi_setup_acquire(&lease) == ESP_ERR_WIFI_STATE && lease == 0,
              "setup cannot enter between reconnect decision and driver call");
    }
    return 1;
}
esp_err_t esp_event_handler_register(esp_event_base_t base, int32_t id, esp_event_handler_t handler, void *context)
{
    if (base == IP_EVENT) { assert(id == IP_EVENT_STA_GOT_IP); ip_handler = handler; ip_context = context; }
    if (base == WIFI_EVENT) { assert(id == ESP_EVENT_ANY_ID); wifi_handler = handler; wifi_context = context; }
    return ESP_OK;
}
void *esp_netif_create_default_wifi_sta(void) { static int netif; return &netif; }
esp_err_t esp_wifi_init(const wifi_init_config_t *config) { (void)config; return ESP_OK; }
esp_err_t esp_wifi_set_storage(int storage) { (void)storage; ++driver_mutations; return ESP_OK; }
esp_err_t esp_wifi_set_mode(int mode) { (void)mode; ++driver_mutations; return ESP_OK; }
esp_err_t esp_wifi_get_config(int iface, wifi_config_t *config)
{ (void)iface; *config = (wifi_config_t){0}; memcpy(config->sta.ssid, "Example Wi-Fi", 13); return ESP_OK; }
esp_err_t esp_wifi_set_config(int iface, const wifi_config_t *config)
{
    (void)iface; (void)config; ++driver_mutations;
    if (inject_disconnect) {
        inject_disconnect = false;
        wifi_event_sta_disconnected_t event = {.reason = 8};
        wifi_handler(wifi_context, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &event);
    }
    return config_result;
}
esp_err_t esp_wifi_start(void) { ++driver_mutations; return ESP_OK; }
esp_err_t esp_wifi_stop(void) { ++driver_mutations; return ESP_OK; }
esp_err_t esp_wifi_restore(void) { ++driver_mutations; return ESP_OK; }
esp_err_t esp_wifi_set_ps(int mode) { (void)mode; ++driver_mutations; return ESP_OK; }
esp_err_t esp_wifi_connect(void) { ++driver_mutations; ++connects; return connect_result; }
esp_err_t esp_wifi_disconnect(void) { ++driver_mutations; return ESP_OK; }
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *record) { ++ap_reads; *record = ap; return ap_result; }
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config, bool block)
{
    (void)config; assert(block);
    if (probe_scan) {
        unsigned before = driver_mutations;
        check(esp_openclaw_node_wifi_set_credentials("Other network", "example-password") == ESP_ERR_WIFI_STATE,
              "credential changes are rejected during a scan");
        check(esp_openclaw_node_wifi_clear_credentials() == ESP_ERR_WIFI_STATE,
              "clearing credentials is rejected during a scan");
        check(esp_openclaw_node_wifi_connect() == ESP_ERR_WIFI_STATE,
              "connecting is rejected during a scan");
        check(esp_openclaw_node_wifi_disconnect() == ESP_ERR_WIFI_STATE,
              "disconnecting is rejected during a scan");
        check(driver_mutations == before, "rejected operations do not reach the driver");
        esp_openclaw_node_wifi_setup_lease_t lease = 0;
        check(esp_openclaw_node_wifi_setup_acquire(&lease) == ESP_ERR_WIFI_STATE,
              "setup cannot replace an active scan");
    }
    if (scan_lease != 0) {
        esp_err_t resumed = ESP_FAIL;
        check(esp_openclaw_node_wifi_setup_release(scan_lease, &resumed) == ESP_ERR_WIFI_STATE && resumed == ESP_FAIL,
              "setup cannot release ownership during its blocking scan");
        wifi_ap_record_t records[1];
        uint16_t count = 1, total = 0;
        check(esp_openclaw_node_wifi_setup_scan(scan_lease, records, &count, &total) == ESP_ERR_WIFI_STATE,
              "a second leased scan cannot overlap");
    }
    return scan_result;
}
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

static void check_mutation_ownership(void)
{
    wifi_ap_record_t records[1];
    uint16_t count = 1, total = 0;
    probe_scan = true;
    check(esp_openclaw_node_wifi_scan(records, &count, &total) == ESP_OK, "scan completes after rejecting competing mutations");
    probe_scan = false;
    esp_openclaw_node_wifi_status_t wifi;
    esp_openclaw_node_wifi_get_status(&wifi);
    check(wifi.has_saved_network && strcmp(wifi.ssid, "Example Wi-Fi") == 0,
          "scan preserves the saved network despite competing callers");

    check(esp_openclaw_node_wifi_set_credentials("Example Wi-Fi", NULL) == ESP_OK, "mutation admitted after scan");
    wifi_event_sta_disconnected_t event = {.reason = 8};
    unsigned before = connects;
    wifi_handler(wifi_context, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &event);
    check(connects == before + 1, "ordinary reconnect resumes after the mutation");

    config_result = ESP_FAIL;
    before = connects;
    inject_disconnect = true;
    check(esp_openclaw_node_wifi_set_credentials("Other network", NULL) == ESP_FAIL, "driver failure is reported");
    check(connects == before + 1, "failed configuration does not discard a concurrent reconnect request");
    config_result = ESP_OK;
    check(esp_openclaw_node_wifi_disconnect() == ESP_OK, "failed mutation releases ownership");
    scan_result = ESP_FAIL;
    count = 1;
    check(esp_openclaw_node_wifi_scan(records, &count, &total) == ESP_FAIL, "scan failure is reported");
    scan_result = ESP_OK;
    check(esp_openclaw_node_wifi_connect() == ESP_OK, "failed scan releases ownership");
}

static void check_setup_ownership(void)
{
    esp_openclaw_node_wifi_setup_lease_t lease = 0, other = 0;
    esp_err_t resumed = ESP_FAIL;
    wifi_ap_record_t records[1];
    uint16_t count = 1, total = 0;
    ip_event_got_ip_t ip = {.ip_info.ip.addr = 0x010200c0};
    wifi_event_sta_disconnected_t disconnected = {.reason = 8};
    ip_handler(ip_context, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    unsigned before = driver_mutations;
    check(esp_openclaw_node_wifi_setup_acquire(NULL) == ESP_ERR_INVALID_ARG, "lease output is required");
    check(esp_openclaw_node_wifi_setup_acquire(&lease) == ESP_OK && lease != 0, "setup obtains a lease");
    check(driver_mutations == before && esp_openclaw_node_wifi_is_connected(), "acquire preserves the live link");
    check(esp_openclaw_node_wifi_setup_acquire(&other) == ESP_ERR_WIFI_STATE && other == 0, "second owner cannot displace setup");
    check(esp_openclaw_node_wifi_set_credentials("Other network", NULL) == ESP_ERR_WIFI_STATE, "setup excludes credential writes");
    check(esp_openclaw_node_wifi_clear_credentials() == ESP_ERR_WIFI_STATE, "setup excludes credential clearing");
    check(esp_openclaw_node_wifi_connect() == ESP_ERR_WIFI_STATE, "setup excludes explicit connect");
    check(esp_openclaw_node_wifi_disconnect() == ESP_ERR_WIFI_STATE, "setup excludes explicit disconnect");
    check(esp_openclaw_node_wifi_scan(records, &count, &total) == ESP_ERR_WIFI_STATE, "setup excludes ordinary scan");
    count = 1;
    check(esp_openclaw_node_wifi_setup_scan(lease, records, &count, &total) == ESP_ERR_WIFI_STATE, "leased scan preserves connected network");
    check(driver_mutations == before, "competing requests never reach driver");
    check(esp_openclaw_node_wifi_setup_release(lease, NULL) == ESP_ERR_INVALID_ARG, "release requires result output");
    check(esp_openclaw_node_wifi_setup_release(lease + 1, &resumed) == ESP_ERR_INVALID_STATE && resumed == ESP_FAIL,
          "unowned release preserves lease and output");

    unsigned retry_before = connects;
    wifi_handler(wifi_context, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnected);
    wifi_handler(wifi_context, WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
    check(connects == retry_before, "lease defers start and disconnect reconnect requests");
    check(esp_openclaw_node_wifi_setup_release(lease, &resumed) == ESP_OK && resumed == ESP_OK,
          "release reports successful reconnect dispatch");
    check(connects == retry_before + 1, "release coalesces deferred reconnects");
    check(esp_openclaw_node_wifi_setup_acquire(&other) == ESP_OK && other != lease, "next owner gets fresh lease");
    count = 1;
    check(esp_openclaw_node_wifi_setup_scan(lease, records, &count, &total) == ESP_ERR_INVALID_STATE, "old lease cannot scan");
    check(esp_openclaw_node_wifi_setup_release(lease, &resumed) == ESP_ERR_INVALID_STATE, "old lease cannot release new owner");
    wifi_handler(wifi_context, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnected);
    ip_handler(ip_context, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    retry_before = connects;
    check(esp_openclaw_node_wifi_setup_release(other, &resumed) == ESP_OK && connects == retry_before,
          "late connected state makes deferred retry unnecessary");

    check(esp_openclaw_node_wifi_setup_acquire(&lease) == ESP_OK, "setup can reopen");
    wifi_handler(wifi_context, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnected);
    connect_result = ESP_FAIL;
    check(esp_openclaw_node_wifi_setup_release(lease, &resumed) == ESP_OK && resumed == ESP_FAIL,
          "reconnect dispatch failure is separate from successful release");
    connect_result = ESP_OK;
    probe_reconnect_unlock = true;
    wifi_handler(wifi_context, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnected);
    probe_reconnect_unlock = true;
    wifi_handler(wifi_context, WIFI_EVENT, WIFI_EVENT_STA_START, NULL);

    check(esp_openclaw_node_wifi_disconnect() == ESP_OK, "explicit disconnect still works after release");
    check(esp_openclaw_node_wifi_setup_acquire(&lease) == ESP_OK, "disconnected setup obtains lease");
    probe_scan = true;
    scan_lease = lease;
    scan_result = ESP_FAIL;
    count = 1;
    check(esp_openclaw_node_wifi_setup_scan(lease, records, &count, &total) == ESP_FAIL, "leased scan reports driver failure");
    scan_result = ESP_OK;
    count = 1;
    check(esp_openclaw_node_wifi_setup_scan(lease, records, &count, &total) == ESP_OK, "failed scan frees operation but retains lease");
    scan_lease = 0;
    probe_scan = false;
    retry_before = connects;
    wifi_handler(wifi_context, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnected);
    check(esp_openclaw_node_wifi_setup_release(lease, &resumed) == ESP_OK && resumed == ESP_OK && connects == retry_before,
          "release preserves explicit disconnect policy");
    check(esp_openclaw_node_wifi_connect() == ESP_OK, "ordinary operations resume after release");
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
    check_mutation_ownership();
    check_setup_ownership();
    printf("Wi-Fi status: %u failed checks\n", failures);
    return failures ? 1 : 0;
}
