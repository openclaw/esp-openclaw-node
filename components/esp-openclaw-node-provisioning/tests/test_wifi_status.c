#include "wifi_host_sdk.h"
#include "esp_openclaw_node_wifi.h"
#include "esp_openclaw_setup_wifi.h"
#include "esp_openclaw_node_common_device_node_cmd.h"
#include "cJSON.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

const char *WIFI_EVENT = "wifi", *IP_EVENT = "ip";
static esp_event_handler_t ip_handler, wifi_handler;
static void *ip_context, *wifi_context;
static esp_netif_t station;
static esp_event_handler_t drain_handler;
static esp_event_base_t drain_base;
static struct queued_event {
    esp_event_base_t base;
    int32_t id;
    uint64_t data[8];
    size_t size;
} queue[32];
static unsigned queued;
static int64_t now_us = 1000000;
static esp_err_t stop_result, deinit_result, post_result, init_result, start_result, ps_result;
static bool initialized, running;
static unsigned config_writes;
static wifi_config_t saved_config;
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
static bool disconnect_on_unlock;
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
    if (disconnect_on_unlock) {
        disconnect_on_unlock = false;
        check(esp_openclaw_node_wifi_disconnect() == ESP_OK, "disconnect can follow an IP status publication");
    }
    return 1;
}
esp_err_t esp_event_handler_register(esp_event_base_t base, int32_t id, esp_event_handler_t handler, void *context)
{
    if (base == IP_EVENT) { assert(id == IP_EVENT_STA_GOT_IP); ip_handler = handler; ip_context = context; }
    if (base == WIFI_EVENT) { assert(id == ESP_EVENT_ANY_ID); wifi_handler = handler; wifi_context = context; }
    if (base != IP_EVENT && base != WIFI_EVENT) {
        assert(id == ESP_EVENT_ANY_ID); drain_handler = handler; drain_base = base;
    }
    return ESP_OK;
}
esp_err_t esp_event_post(esp_event_base_t base, int32_t id, const void *data, size_t size, TickType_t wait)
{
    (void)wait;
    if (post_result != ESP_OK) return post_result;
    assert(queued < 32 && size <= sizeof(queue[0].data));
    queue[queued].base = base; queue[queued].id = id; queue[queued].size = size;
    memcpy(queue[queued++].data, data, size);
    return ESP_OK;
}
#if SOC_WIFI_SUPPORTED && !CONFIG_ESP_WIFI_REMOTE_ENABLED && !CONFIG_ESP_HOST_WIFI_ENABLED
static void dispatch_one(void)
{
    assert(queued);
    struct queued_event event = queue[0];
    memmove(queue, queue + 1, --queued * sizeof(queue[0]));
    if (event.base == drain_base) drain_handler(NULL, event.base, event.id, event.data);
    else if (event.base == IP_EVENT) ip_handler(ip_context, event.base, event.id, event.data);
    else wifi_handler(wifi_context, event.base, event.id, event.data);
}
static void dispatch_all(void) { while (queued) dispatch_one(); }
#endif
esp_netif_t *esp_netif_create_default_wifi_sta(void) { return &station; }
esp_err_t esp_wifi_init(const wifi_init_config_t *config)
{
    (void)config; assert(!initialized);
    if (init_result == ESP_OK) initialized = true;
    return init_result;
}
esp_err_t esp_wifi_deinit(void)
{
    assert(initialized && !running); ++driver_mutations;
    if (deinit_result == ESP_OK) initialized = false;
    return deinit_result;
}
esp_err_t esp_wifi_set_storage(int storage) { (void)storage; ++driver_mutations; return ESP_OK; }
esp_err_t esp_wifi_set_mode(int mode) { (void)mode; ++driver_mutations; return ESP_OK; }
esp_err_t esp_wifi_get_config(int iface, wifi_config_t *config)
{ (void)iface; *config = (wifi_config_t){0}; memcpy(config->sta.ssid, "Example Wi-Fi", 13); return ESP_OK; }
esp_err_t esp_wifi_set_config(int iface, const wifi_config_t *config)
{
    (void)iface; ++driver_mutations; ++config_writes;
    if (config_result == ESP_OK) saved_config = *config;
    if (inject_disconnect) {
        inject_disconnect = false;
        wifi_event_sta_disconnected_t event = {.reason = 8};
        wifi_handler(wifi_context, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &event);
    }
    return config_result;
}
esp_err_t esp_wifi_start(void) { assert(initialized); ++driver_mutations; if (start_result == ESP_OK) running = true; return start_result; }
esp_err_t esp_wifi_stop(void) { assert(initialized); ++driver_mutations; if (stop_result == ESP_OK) running = false; return stop_result; }
esp_err_t esp_wifi_restore(void) { ++driver_mutations; return ESP_OK; }
esp_err_t esp_wifi_set_ps(int mode) { (void)mode; ++driver_mutations; return ps_result; }
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
int64_t esp_timer_get_time(void) { return now_us; }

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
    ip_event_got_ip_t ip = {.esp_netif = &station, .ip_info.ip.addr = 0x010200c0};
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

static void check_late_ip_after_disconnect(void)
{
    ip_event_got_ip_t ip = {.esp_netif = &station, .ip_info.ip.addr = 0x010200c0};
    check(esp_openclaw_node_wifi_disconnect() == ESP_OK, "explicit disconnect accepted");
    ip_handler(ip_context, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    esp_openclaw_node_wifi_status_t wifi;
    esp_openclaw_node_wifi_get_status(&wifi);
    check(!wifi.connected && wifi.ip[0] == '\0', "queued IP cannot resurrect explicitly disconnected status");
    check(!esp_openclaw_node_wifi_wait_for_connection(0), "queued IP cannot wake a connection waiter after disconnect");

    check(esp_openclaw_node_wifi_connect() == ESP_OK, "explicit reconnect accepted");
    ip_handler(ip_context, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    check(esp_openclaw_node_wifi_is_connected() && esp_openclaw_node_wifi_wait_for_connection(0),
          "newly desired connection can publish IP and wake waiters");

    disconnect_on_unlock = true;
    ip_handler(ip_context, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    check(!esp_openclaw_node_wifi_is_connected() && !esp_openclaw_node_wifi_wait_for_connection(0),
          "disconnect between status publication and waiter signaling cannot leave a stale connected bit");

    check(esp_openclaw_node_wifi_clear_credentials() == ESP_OK, "credential clearing accepted");
    ip_handler(ip_context, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    esp_openclaw_node_wifi_get_status(&wifi);
    check(!wifi.has_saved_network && !wifi.connected && wifi.ip[0] == '\0',
          "queued IP cannot resurrect a forgotten network");
    check(!esp_openclaw_node_wifi_wait_for_connection(0), "forgotten network cannot wake connection waiters");
}

static bool authorized(void *context) { return *(bool *)context; }

#if SOC_WIFI_SUPPORTED && !CONFIG_ESP_WIFI_REMOTE_ENABLED && !CONFIG_ESP_HOST_WIFI_ENABLED
static esp_openclaw_node_wifi_attempt_t poll_join(uint64_t lease, uint64_t id, bool allowed)
{
    esp_openclaw_node_wifi_attempt_t result = {0};
    assert(esp_openclaw_node_wifi_setup_join_poll(lease, id, authorized, &allowed, &result) == ESP_OK);
    return result;
}

static esp_openclaw_node_wifi_attempt_t advance_join(uint64_t lease, uint64_t id,
    esp_openclaw_node_wifi_attempt_state_t state, bool allowed)
{
    for (unsigned i = 0; i < 40; ++i) {
        esp_openclaw_node_wifi_attempt_t result = poll_join(lease, id, allowed);
        if (result.state == state) return result;
        dispatch_all();
    }
    assert(!"attempt did not reach expected outcome");
    return (esp_openclaw_node_wifi_attempt_t){0};
}

static bool revoke_before_write(void *context)
{
    unsigned *calls = context;
    return ++*calls == 1;
}

static uint64_t begin_join(uint64_t lease)
{
    uint64_t id = 0;
    assert(esp_openclaw_node_wifi_setup_join_begin(lease, "Setup network", "example-password", &id) == ESP_OK);
    assert(id != 0);
    return id;
}

static void advance_to_barrier(uint64_t lease, uint64_t id)
{
    for (unsigned i = 0; i < 10 && !queued; ++i) poll_join(lease, id, true);
    assert(queued && queue[0].base == drain_base);
}
#endif

static void check_setup_attempts(void)
{
    uint64_t lease = 0, id = 0;
    assert(esp_openclaw_node_wifi_setup_acquire(&lease) == ESP_OK);
#if SOC_WIFI_SUPPORTED && !CONFIG_ESP_WIFI_REMOTE_ENABLED && !CONFIG_ESP_HOST_WIFI_ENABLED
    ip_event_got_ip_t ip = {.esp_netif = &station, .ip_info.ip.addr = 0x010200c0};
    wifi_event_sta_disconnected_t disconnected = {.reason = 201};
    esp_err_t resumed;
    unsigned before = driver_mutations;
    id = begin_join(lease);
    check(esp_openclaw_node_wifi_setup_join_begin(lease, "Other", NULL, &id) == ESP_ERR_WIFI_STATE,
          "overlapping attempt is rejected without replacing its ID");
    check(driver_mutations == before, "begin only queues credentials");
    check(esp_openclaw_node_wifi_setup_release(lease, &resumed) == ESP_ERR_WIFI_STATE,
          "active attempt retains ownership");
    check(esp_openclaw_node_wifi_setup_join_cancel(lease, id) == ESP_OK, "queued attempt can cancel");
    check(poll_join(lease, id, true).state == ESP_OPENCLAW_NODE_WIFI_CANCELLED && driver_mutations == before,
          "cancel before start does not touch the radio");

    uint64_t old_id = id;
    id = begin_join(lease);
    check(id != old_id && esp_openclaw_node_wifi_setup_join_cancel(lease, old_id) == ESP_ERR_INVALID_STATE,
          "stale attempt cannot cancel its replacement");
    unsigned writes = config_writes;
    advance_to_barrier(lease, id);
    struct queued_event stale_marker = queue[0];
    /* A prior connection's queued events, including IP queued by another event,
     * must drain before a new configuration is committed. */
    wifi_handler(wifi_context, WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL);
    ip_handler(ip_context, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    dispatch_one();
    assert(esp_event_post(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip, sizeof(ip), 0) == ESP_OK);
    poll_join(lease, id, true);
    check(config_writes == writes && !esp_openclaw_node_wifi_is_connected(),
          "first barrier is not sufficient to apply credentials or accept old IP");
    dispatch_one();
    poll_join(lease, id, true);
    check(config_writes == writes, "nested old IP remains fenced behind second barrier");
    dispatch_all();
    advance_join(lease, id, ESP_OPENCLAW_NODE_WIFI_JOINING, true);
    check(config_writes == writes + 1 && strcmp((char *)saved_config.sta.ssid, "Setup network") == 0,
          "configuration is applied after both barriers");
    ip_handler(ip_context, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    check(poll_join(lease, id, true).state == ESP_OPENCLAW_NODE_WIFI_JOINING,
          "IP without this attempt's association cannot complete join");
    wifi_handler(wifi_context, WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL);
    esp_netif_t other_station;
    ip.esp_netif = &other_station;
    ip_handler(ip_context, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    check(poll_join(lease, id, true).state == ESP_OPENCLAW_NODE_WIFI_JOINING,
          "another netif cannot complete join");
    ip.esp_netif = &station;
    ip_handler(ip_context, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    esp_openclaw_node_wifi_attempt_t result = poll_join(lease, id, true);
    check(result.state == ESP_OPENCLAW_NODE_WIFI_SUCCEEDED && result.credentials_saved,
          "own association and IP complete an authorized join");
    check(esp_openclaw_node_wifi_setup_join_cancel(lease, id) == ESP_OK &&
          poll_join(lease, id, false).state == ESP_OPENCLAW_NODE_WIFI_SUCCEEDED,
          "terminal receipt cannot be rewritten by cancellation");
    check(esp_openclaw_node_wifi_setup_release(lease, &resumed) == ESP_OK &&
          esp_openclaw_node_wifi_is_connected(), "successful link transfers to ordinary ownership");

    assert(esp_openclaw_node_wifi_setup_acquire(&lease) == ESP_OK);
    id = begin_join(lease);
    before = driver_mutations;
    check(poll_join(lease, id, false).state == ESP_OPENCLAW_NODE_WIFI_CANCELLED &&
          driver_mutations == before && esp_openclaw_node_wifi_is_connected(),
          "revoked queued work preserves existing connection");
    id = begin_join(lease);
    advance_join(lease, id, ESP_OPENCLAW_NODE_WIFI_JOINING, true);
    wifi_handler(wifi_context, WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL);
    ip_handler(ip_context, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    check(poll_join(lease, id, false).state == ESP_OPENCLAW_NODE_WIFI_DRAINING,
          "authority lost after IP cancels instead of reporting success");
    advance_join(lease, id, ESP_OPENCLAW_NODE_WIFI_CANCELLED, false);
    check(!esp_openclaw_node_wifi_is_connected(), "revocation drains even without authority");

    id = begin_join(lease);
    advance_join(lease, id, ESP_OPENCLAW_NODE_WIFI_JOINING, true);
    wifi_handler(wifi_context, WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL);
    wifi_handler(wifi_context, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnected);
    wifi_handler(wifi_context, WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL);
    ip_handler(ip_context, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    check(!esp_openclaw_node_wifi_is_connected() &&
          poll_join(lease, id, true).state == ESP_OPENCLAW_NODE_WIFI_DRAINING,
          "disconnect remains latched despite a later association and IP");
    result = advance_join(lease, id, ESP_OPENCLAW_NODE_WIFI_FAILED, true);
    check(result.error == ESP_ERR_WIFI_CONN && result.disconnect_reason == 201 && result.credentials_saved,
          "failed receipt keeps join error and saved-credential outcome");

    id = begin_join(lease);
    advance_join(lease, id, ESP_OPENCLAW_NODE_WIFI_JOINING, true);
    now_us += (int64_t)ESP_OPENCLAW_NODE_WIFI_ATTEMPT_TIMEOUT_MS * 1000;
    check(poll_join(lease, id, true).state == ESP_OPENCLAW_NODE_WIFI_DRAINING,
          "join timeout is not drain completion");
    result = advance_join(lease, id, ESP_OPENCLAW_NODE_WIFI_FAILED, true);
    check(result.error == ESP_ERR_TIMEOUT, "join deadline produces bounded failed receipt");

    /* Failure at each lifecycle boundary must retain ownership without retrying
     * until the owner explicitly requests recovery. The fake driver rejects
     * deinit while running and init over an existing instance. */
    esp_err_t *failures_at[] = {&stop_result, &deinit_result, &post_result,
        &init_result, &start_result, &ps_result};
    for (size_t i = 0; i < sizeof(failures_at) / sizeof(failures_at[0]); ++i) {
        id = begin_join(lease);
        *failures_at[i] = ESP_FAIL;
        result = advance_join(lease, id, ESP_OPENCLAW_NODE_WIFI_RECOVERY_REQUIRED, true);
        before = driver_mutations;
        check(result.recovery_error == ESP_FAIL &&
              esp_openclaw_node_wifi_setup_release(lease, &resumed) == ESP_ERR_WIFI_STATE,
              "failed drain retains ownership with recovery reason");
        poll_join(lease, id, true);
        poll_join(lease, id, true);
        check(driver_mutations == before, "blocked drain does not retry driver calls automatically");
        *failures_at[i] = ESP_OK;
        assert(esp_openclaw_node_wifi_setup_join_retry_drain(lease, id) == ESP_OK);
        advance_join(lease, id, ESP_OPENCLAW_NODE_WIFI_FAILED, true);
    }

    id = begin_join(lease);
    advance_to_barrier(lease, id);
    struct queued_event old_round = queue[0];
    now_us += (int64_t)ESP_OPENCLAW_NODE_WIFI_ATTEMPT_TIMEOUT_MS * 1000;
    check(poll_join(lease, id, true).state == ESP_OPENCLAW_NODE_WIFI_RECOVERY_REQUIRED,
          "unprocessed barrier times out without releasing ownership");
    assert(esp_openclaw_node_wifi_setup_join_retry_drain(lease, id) == ESP_OK);
    dispatch_all();
    advance_to_barrier(lease, id);
    unsigned pending = queued;
    drain_handler(NULL, drain_base, old_round.id, old_round.data);
    drain_handler(NULL, drain_base, stale_marker.id, stale_marker.data);
    poll_join(lease, id, true);
    check(queued == pending, "old attempt and drain generation cannot advance the new barrier");
    advance_join(lease, id, ESP_OPENCLAW_NODE_WIFI_FAILED, true);

    esp_err_t *join_failures[] = {&config_result, &connect_result};
    for (size_t i = 0; i < sizeof(join_failures) / sizeof(join_failures[0]); ++i) {
        id = begin_join(lease);
        *join_failures[i] = ESP_FAIL;
        result = advance_join(lease, id, ESP_OPENCLAW_NODE_WIFI_FAILED, true);
        check(result.error == ESP_FAIL && result.credentials_saved == (i == 1),
              "write and connect failures drain and report whether credentials were saved");
        *join_failures[i] = ESP_OK;
    }

    /* Revocation at the final write boundary is checked after the general poll
     * check as well. A callback that changes its answer models a closing owner. */
    id = begin_join(lease);
    writes = config_writes;
    advance_to_barrier(lease, id);
    dispatch_all();
    poll_join(lease, id, true);
    dispatch_all();
    poll_join(lease, id, true); /* restore station */
    unsigned authorization_calls = 0;
    assert(esp_openclaw_node_wifi_setup_join_poll(lease, id, revoke_before_write,
        &authorization_calls, &result) == ESP_OK);
    check(authorization_calls == 2 && result.state == ESP_OPENCLAW_NODE_WIFI_DRAINING,
          "authority is rechecked immediately before credential commit");
    advance_join(lease, id, ESP_OPENCLAW_NODE_WIFI_CANCELLED, false);
    check(config_writes == writes, "revoked setup never writes credentials");
    before = connects;
    check(esp_openclaw_node_wifi_setup_release(lease, &resumed) == ESP_OK && connects == before,
          "failed or cancelled setup does not restart automatic joining");
    check(esp_openclaw_node_wifi_connect() == ESP_OK, "owner may explicitly reconnect after cleanup");
#else
    bool allowed = true;
    esp_openclaw_node_wifi_attempt_t result = {0};
    unsigned before = driver_mutations;
    check(esp_openclaw_node_wifi_setup_join_begin(lease, "Setup network", NULL, &id) == ESP_ERR_NOT_SUPPORTED && id == 0,
          "remote backend rejects native drain without accepting credentials");
    check(esp_openclaw_node_wifi_setup_join_poll(lease, 1, authorized, &allowed, &result) == ESP_ERR_NOT_SUPPORTED &&
          esp_openclaw_node_wifi_setup_join_cancel(lease, 1) == ESP_ERR_NOT_SUPPORTED &&
          esp_openclaw_node_wifi_setup_join_retry_drain(lease, 1) == ESP_ERR_NOT_SUPPORTED && driver_mutations == before,
          "remote lifecycle APIs remain side-effect free");
    esp_err_t resumed;
    assert(esp_openclaw_node_wifi_setup_release(lease, &resumed) == ESP_OK);
#endif
}

typedef struct {
    esp_openclaw_setup_session_t session;
    esp_openclaw_setup_wifi_t wifi;
    uint64_t generation, now, tick;
    bool authenticated;
    unsigned cancellations;
} setup_fixture_t;

static uint64_t setup_now(void *context)
{
    setup_fixture_t *f = context;
    uint64_t now = f->now;
    f->now += f->tick;
    return now;
}

static bool setup_peer(uint64_t peer, void *context)
{
    return peer == 1 && ((setup_fixture_t *)context)->authenticated;
}

static void setup_cancel(esp_openclaw_setup_token_t token, void *context)
{
    setup_fixture_t *f = context;
    ++f->cancellations;
    esp_openclaw_setup_wifi_cancel(&f->wifi, token);
}

static void setup_open_confirm(setup_fixture_t *f)
{
    assert(esp_openclaw_setup_open(&f->session, &f->generation) == ESP_OPENCLAW_SETUP_OK);
    assert(esp_openclaw_setup_attach(&f->session, f->generation, 1) == ESP_OPENCLAW_SETUP_OK);
    check(esp_openclaw_setup_wifi_activate(&f->wifi, f->generation) == ESP_ERR_INVALID_STATE,
          "binding cannot take ownership before physical confirmation");
    assert(esp_openclaw_setup_input_released(&f->session, f->generation) == ESP_OPENCLAW_SETUP_OK);
    assert(esp_openclaw_setup_confirm(&f->session, f->generation) == ESP_OPENCLAW_SETUP_OK);
    assert(esp_openclaw_setup_wifi_activate(&f->wifi, f->generation) == ESP_OK);
}

#if SOC_WIFI_SUPPORTED && !CONFIG_ESP_WIFI_REMOTE_ENABLED && !CONFIG_ESP_HOST_WIFI_ENABLED
static esp_openclaw_setup_receipt_t setup_submit(setup_fixture_t *f, uint32_t id)
{
    esp_openclaw_setup_receipt_t receipt;
    assert(esp_openclaw_setup_wifi_submit(&f->wifi, f->generation, 1, id,
        "Setup network", "example-password", &receipt) == ESP_OPENCLAW_SETUP_ACCEPTED);
    return receipt;
}

static esp_openclaw_setup_wifi_snapshot_t setup_advance(setup_fixture_t *f,
    esp_openclaw_node_wifi_attempt_state_t state)
{
    for (unsigned i = 0; i < 40; ++i) {
        assert(esp_openclaw_setup_wifi_poll(&f->wifi) == ESP_OK);
        esp_openclaw_setup_wifi_snapshot_t snapshot = esp_openclaw_setup_wifi_snapshot(&f->wifi);
        if (snapshot.attempt.state == state) return snapshot;
        dispatch_all();
    }
    assert(!"binding did not reach expected state");
    return (esp_openclaw_setup_wifi_snapshot_t){0};
}

static esp_openclaw_setup_receipt_t setup_receipt(setup_fixture_t *f, uint32_t id)
{
    esp_openclaw_setup_receipt_t receipt;
    assert(esp_openclaw_setup_get_receipt(&f->session, f->generation, 1, id, &receipt) == ESP_OPENCLAW_SETUP_OK);
    return receipt;
}
#endif

static void check_setup_binding(void)
{
    setup_fixture_t f = {.now = 100, .authenticated = true};
    esp_openclaw_setup_callbacks_t callbacks = {
        .now_ms = setup_now, .peer_is_current_authenticated = setup_peer,
        .cancel = setup_cancel, .context = &f,
    };
    assert(esp_openclaw_setup_init(&f.session, &callbacks));
    assert(esp_openclaw_setup_wifi_init(&f.wifi, &f.session));
    setup_open_confirm(&f);
    unsigned before = driver_mutations;
    check(esp_openclaw_setup_wifi_activate(&f.wifi, f.generation) == ESP_OK,
          "repeated activation preserves the same lease");
    esp_openclaw_setup_receipt_t receipt;
    check(esp_openclaw_setup_wifi_submit(&f.wifi, f.generation, 2, 1,
          "Other network", NULL, &receipt) == ESP_OPENCLAW_SETUP_UNAUTHORIZED,
          "binding enforces the confirmed peer");
    check(driver_mutations == before, "admission does not mutate the radio");
    check(esp_openclaw_node_wifi_connect() == ESP_ERR_WIFI_STATE,
          "confirmed session excludes console writes before any operation");
    assert(esp_openclaw_setup_wifi_submit(&f.wifi, f.generation, 1, 1,
        NULL, NULL, &receipt) == ESP_OPENCLAW_SETUP_ACCEPTED);
#if SOC_WIFI_SUPPORTED && !CONFIG_ESP_WIFI_REMOTE_ENABLED && !CONFIG_ESP_HOST_WIFI_ENABLED
    check(receipt.outcome == ESP_OPENCLAW_SETUP_FAILED && receipt.detail == ESP_ERR_INVALID_ARG,
          "synchronous validation rejection updates the returned receipt");
#else
    check(receipt.outcome == ESP_OPENCLAW_SETUP_FAILED && receipt.detail == ESP_ERR_NOT_SUPPORTED,
          "remote backend rejection updates the returned receipt");
#endif
    esp_openclaw_setup_receipt_t duplicate;
    check(esp_openclaw_setup_wifi_submit(&f.wifi, f.generation, 1, 1,
          "Changed draft", NULL, &duplicate) == ESP_OPENCLAW_SETUP_DUPLICATE &&
          duplicate.outcome == receipt.outcome && duplicate.detail == receipt.detail,
          "synchronous failure is retained for duplicate requests");
    check(!esp_openclaw_setup_wifi_snapshot(&f.wifi).operation_pending && driver_mutations == before,
          "immediate rejection never leaves pending radio work");

#if SOC_WIFI_SUPPORTED && !CONFIG_ESP_WIFI_REMOTE_ENABLED && !CONFIG_ESP_HOST_WIFI_ENABLED
    receipt = setup_submit(&f, 2);
    esp_openclaw_setup_wifi_snapshot_t pending = esp_openclaw_setup_wifi_snapshot(&f.wifi);
    check(receipt.outcome == ESP_OPENCLAW_SETUP_PENDING && pending.operation_pending,
          "accepted credentials remain pending until the actual connection");
    check(esp_openclaw_setup_wifi_submit(&f.wifi, f.generation, 1, 2,
          "Changed draft", NULL, &duplicate) == ESP_OPENCLAW_SETUP_DUPLICATE &&
          esp_openclaw_setup_wifi_snapshot(&f.wifi).attempt.id == pending.attempt.id,
          "duplicate request cannot start another Wi-Fi attempt");
    check(esp_openclaw_setup_wifi_submit(&f.wifi, f.generation, 1, 3,
          "Other network", NULL, &duplicate) == ESP_OPENCLAW_SETUP_OCCUPIED,
          "second operation cannot overlap the pending join");
    setup_advance(&f, ESP_OPENCLAW_NODE_WIFI_JOINING);
    check(setup_receipt(&f, 2).outcome == ESP_OPENCLAW_SETUP_PENDING,
          "stored credentials are not a successful setup receipt");
    ip_event_got_ip_t ip = {.esp_netif = &station, .ip_info.ip.addr = 0x010200c0};
    wifi_handler(wifi_context, WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL);
    ip_handler(ip_context, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    esp_openclaw_setup_wifi_snapshot_t snapshot = setup_advance(&f, ESP_OPENCLAW_NODE_WIFI_SUCCEEDED);
    check(!snapshot.operation_pending && snapshot.lease_held &&
          setup_receipt(&f, 2).outcome == ESP_OPENCLAW_SETUP_SUCCEEDED,
          "terminal Wi-Fi success completes the session but retains its lease");
    check(esp_openclaw_node_wifi_disconnect() == ESP_ERR_WIFI_STATE,
          "console cannot displace a confirmed session between operations");
    check(esp_openclaw_setup_wifi_submit(&f.wifi, f.generation, 1, 2, "Other", NULL, &duplicate) ==
          ESP_OPENCLAW_SETUP_DUPLICATE && duplicate.outcome == ESP_OPENCLAW_SETUP_SUCCEEDED &&
          esp_openclaw_setup_wifi_snapshot(&f.wifi).attempt.id == snapshot.attempt.id,
          "successful duplicate returns its receipt without reconnecting");
    esp_openclaw_setup_token_t old_token = receipt.token;
    esp_openclaw_setup_close(&f.session);
    assert(esp_openclaw_setup_wifi_poll(&f.wifi) == ESP_OK);
    check(!esp_openclaw_setup_wifi_snapshot(&f.wifi).lease_held && esp_openclaw_node_wifi_is_connected(),
          "closing an idle successful session hands back the live link");

    setup_open_confirm(&f);
    receipt = setup_submit(&f, 1);
    before = driver_mutations;
    esp_openclaw_setup_wifi_cancel(&f.wifi, old_token);
    esp_openclaw_setup_close(&f.session);
    assert(esp_openclaw_setup_wifi_poll(&f.wifi) == ESP_OK);
    snapshot = esp_openclaw_setup_wifi_snapshot(&f.wifi);
    check(snapshot.attempt.state == ESP_OPENCLAW_NODE_WIFI_CANCELLED && !snapshot.lease_held &&
          esp_openclaw_node_wifi_is_connected() && driver_mutations == before,
          "queued cancellation closes and releases without disturbing the existing link");
    old_token = receipt.token;

    setup_open_confirm(&f);
    receipt = setup_submit(&f, 1);
    esp_openclaw_setup_wifi_cancel(&f.wifi, old_token);
    assert(esp_openclaw_setup_wifi_poll(&f.wifi) == ESP_OK);
    check(esp_openclaw_setup_wifi_snapshot(&f.wifi).operation_pending,
          "stale-generation cancellation cannot cancel a replacement operation");
    setup_advance(&f, ESP_OPENCLAW_NODE_WIFI_JOINING);
    esp_openclaw_setup_wifi_cancel(&f.wifi, receipt.token);
    assert(esp_openclaw_setup_wifi_poll(&f.wifi) == ESP_OK);
    check(setup_receipt(&f, 1).outcome == ESP_OPENCLAW_SETUP_PENDING,
          "cancellation remains pending while the radio drains");
    snapshot = setup_advance(&f, ESP_OPENCLAW_NODE_WIFI_CANCELLED);
    check(snapshot.attempt.credentials_saved && snapshot.lease_held && !snapshot.operation_pending &&
          setup_receipt(&f, 1).detail == ESP_OPENCLAW_SETUP_WIFI_CANCELLED,
          "drained cancellation has a distinct receipt and retains saved-credential evidence");

    setup_submit(&f, 2);
    esp_openclaw_setup_wifi_cancel(&f.wifi, receipt.token);
    setup_advance(&f, ESP_OPENCLAW_NODE_WIFI_JOINING);
    check(esp_openclaw_setup_wifi_snapshot(&f.wifi).operation_pending,
          "old operation ID in the same session cannot cancel its replacement");
    wifi_event_sta_disconnected_t disconnected = {.reason = 201};
    wifi_handler(wifi_context, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnected);
    assert(esp_openclaw_setup_wifi_poll(&f.wifi) == ESP_OK);
    check(setup_receipt(&f, 2).outcome == ESP_OPENCLAW_SETUP_PENDING,
          "disconnect cannot complete the session before drain");
    setup_advance(&f, ESP_OPENCLAW_NODE_WIFI_FAILED);
    check(setup_receipt(&f, 2).detail == ESP_ERR_WIFI_CONN,
          "connection error reaches the correlated session receipt");

    setup_submit(&f, 3);
    setup_advance(&f, ESP_OPENCLAW_NODE_WIFI_JOINING);
    now_us += (int64_t)ESP_OPENCLAW_NODE_WIFI_ATTEMPT_TIMEOUT_MS * 1000;
    assert(esp_openclaw_setup_wifi_poll(&f.wifi) == ESP_OK);
    check(setup_receipt(&f, 3).outcome == ESP_OPENCLAW_SETUP_PENDING,
          "Wi-Fi timeout waits for drain before receipt completion");
    setup_advance(&f, ESP_OPENCLAW_NODE_WIFI_FAILED);
    check(setup_receipt(&f, 3).detail == ESP_ERR_TIMEOUT, "timeout detail survives session completion");

    setup_submit(&f, 4);
    setup_advance(&f, ESP_OPENCLAW_NODE_WIFI_JOINING);
    stop_result = ESP_FAIL;
    esp_openclaw_setup_close(&f.session);
    snapshot = setup_advance(&f, ESP_OPENCLAW_NODE_WIFI_RECOVERY_REQUIRED);
    check(snapshot.operation_pending && snapshot.lease_held &&
          esp_openclaw_setup_snapshot(&f.session).phase == ESP_OPENCLAW_SETUP_CLOSING,
          "failed drain retains the closing session and lease");
    check(esp_openclaw_node_wifi_connect() == ESP_ERR_WIFI_STATE,
          "failed cleanup cannot return the radio to console callers");
    check(esp_openclaw_setup_open(&f.session, &f.generation) == ESP_OPENCLAW_SETUP_OCCUPIED,
          "new setup cannot replace a closing failed drain");
    stop_result = ESP_OK;
    assert(esp_openclaw_setup_wifi_retry_drain(&f.wifi) == ESP_OK);
    snapshot = setup_advance(&f, ESP_OPENCLAW_NODE_WIFI_CANCELLED);
    check(!snapshot.operation_pending && !snapshot.lease_held && snapshot.attempt.credentials_saved &&
          esp_openclaw_setup_snapshot(&f.session).phase == ESP_OPENCLAW_SETUP_CLOSED,
          "explicit cleanup recovery closes and releases while retaining the final attempt");

    setup_open_confirm(&f);
    setup_submit(&f, 1);
    before = driver_mutations;
    unsigned canceled = f.cancellations;
    /* The outer poll sees a live session; the Wi-Fi authorizer observes expiry. */
    f.tick = ESP_OPENCLAW_SETUP_TIMEOUT_MS;
    assert(esp_openclaw_setup_wifi_poll(&f.wifi) == ESP_OK);
    f.tick = 0;
    snapshot = esp_openclaw_setup_wifi_snapshot(&f.wifi);
    check(f.cancellations == canceled + 1 && !snapshot.operation_pending && !snapshot.lease_held &&
          snapshot.attempt.state == ESP_OPENCLAW_NODE_WIFI_CANCELLED && driver_mutations == before,
          "expiry inside authorization safely cancels queued work and releases ownership");
#else
    esp_openclaw_setup_close(&f.session);
    assert(esp_openclaw_setup_wifi_poll(&f.wifi) == ESP_OK);
    check(!esp_openclaw_setup_wifi_snapshot(&f.wifi).lease_held,
          "unsupported backend releases its lease after session close");
#endif
    /* A release can succeed while its deferred reconnect dispatch fails. */
    assert(esp_openclaw_node_wifi_set_credentials("Original network", NULL) == ESP_OK);
    setup_open_confirm(&f);
    wifi_event_sta_disconnected_t lost = {.reason = 8};
    wifi_handler(wifi_context, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &lost);
    connect_result = ESP_FAIL;
    esp_openclaw_setup_close(&f.session);
    check(esp_openclaw_setup_wifi_poll(&f.wifi) == ESP_FAIL &&
          !esp_openclaw_setup_wifi_snapshot(&f.wifi).lease_held,
          "release reports reconnect dispatch failure without leaking admission ownership");
    connect_result = ESP_OK;
}

int main(void)
{
    assert(esp_openclaw_node_register_device_commands(NULL, NULL) == ESP_OK);
    cJSON *result = status();
    check(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(result, "connected")), "uninitialized Wi-Fi is disconnected");
    check(!cJSON_HasObjectItem(result, "rssi"), "disconnected status omits AP observations");
    cJSON_Delete(result);

    assert(esp_openclaw_node_wifi_start() == ESP_OK);
    ip_event_got_ip_t event = {.esp_netif = &station, .ip_info.ip.addr = 0x010200c0};
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
    check_late_ip_after_disconnect();
    check_setup_attempts();
    check_setup_binding();
    printf("Wi-Fi status and setup: %u failed checks\n", failures);
    return failures ? 1 : 0;
}
