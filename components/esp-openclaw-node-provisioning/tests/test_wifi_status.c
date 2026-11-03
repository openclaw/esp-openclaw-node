#include "wifi_host_sdk.h"
#include "esp_openclaw_node_wifi.h"
#include "esp_openclaw_setup_wifi.h"
#include "esp_openclaw_setup_dispatcher.h"
#include "esp_openclaw_setup_mailbox.h"
#include "esp_openclaw_setup_security2.h"
#include "esp_openclaw_setup_runtime.h"
#include "protocomm_security2.h"
#include "protocomm_ble.h"
#include "esp_srp.h"
#include "freertos/task.h"
#include "esp_openclaw_node_common_device_node_cmd.h"
#include "cJSON.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

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
static uint16_t scan_found;
static void (*during_scan)(void);
static pthread_mutex_t wifi_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned random_calls;
static bool seed_expected;
static esp_err_t get_ps_result;
static wifi_ps_type_t power_save = WIFI_PS_NONE;
static unsigned power_reads;

void esp_fill_random(void *output, size_t size)
{
    if (seed_expected) {
        assert(size == 32 && initialized && running);
        assert(pthread_mutex_trylock(&wifi_mutex) == EBUSY);
    }
    memset(output, (int)++random_calls, size);
}

static void check(bool condition, const char *message);

const char *esp_err_to_name(esp_err_t error) { (void)error; return "test error"; }
EventGroupHandle_t xEventGroupCreate(void) { return &event_bits; }
EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits) { (void)group; event_bits &= ~bits; return event_bits; }
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits) { (void)group; event_bits |= bits; return event_bits; }
EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits, int clear, int all, TickType_t timeout)
{ (void)group; (void)bits; (void)clear; (void)all; (void)timeout; return event_bits; }
SemaphoreHandle_t xSemaphoreCreateMutex(void)
{ return &wifi_mutex; }
int xSemaphoreTake(SemaphoreHandle_t lock, TickType_t timeout)
{ (void)timeout; assert(pthread_mutex_lock(lock) == 0); return 1; }
int xSemaphoreGive(SemaphoreHandle_t lock)
{
    assert(pthread_mutex_unlock(lock) == 0);
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
esp_err_t esp_wifi_get_ps(wifi_ps_type_t *mode) { ++power_reads; *mode = power_save; return get_ps_result; }
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
        uint8_t seed[32] = {0x72}; unsigned reads = power_reads, calls = random_calls;
        esp_err_t seed_result = esp_openclaw_node_wifi_setup_seed(seed);
        check((seed_result == ESP_ERR_WIFI_STATE || seed_result == ESP_ERR_NOT_SUPPORTED) &&
              reads == power_reads && calls == random_calls && seed[0] == 0x72,
              "busy/unsupported entropy reads neither SDK nor output");
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
    if (during_scan) during_scan();
    return scan_result;
}
esp_err_t esp_wifi_scan_get_ap_num(uint16_t *count) { *count = scan_found; return ESP_OK; }
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *count, wifi_ap_record_t *records)
{
    if (*count > scan_found) *count = scan_found;
    for (unsigned i = 0; i < *count; ++i) {
        records[i] = (wifi_ap_record_t){.rssi = -55, .primary = 6, .authmode = WIFI_AUTH_WPA2_PSK};
        memset(records[i].ssid, 's', 32);
    }
    return ESP_OK;
}
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
    uint8_t seed[32] = {0x72}; unsigned reads = power_reads, calls = random_calls;
    assert(esp_openclaw_node_wifi_setup_seed(seed) == ESP_ERR_INVALID_STATE &&
        reads == power_reads && calls == random_calls && seed[0] == 0x72);
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

/* Gateway API boundary only. Native admission/worker/authority/drain behavior is
 * exercised by the existing run_connect_lease_host_tests.py production lane. */
struct esp_openclaw_node { unsigned unused; };
static struct esp_openclaw_node gateway_node;
static esp_openclaw_node_connect_lease_t gateway_lease, next_gateway_lease;
static esp_openclaw_node_connect_attempt_t gateway_attempt;
static unsigned gateway_begins;
static esp_err_t gateway_acquire_error;
static esp_openclaw_node_connect_authorize_t gateway_authority;
static void *gateway_authority_context;
static char gateway_code[4097];
esp_err_t esp_openclaw_node_connect_lease_acquire(esp_openclaw_node_handle_t node, esp_openclaw_node_connect_lease_t *lease)
{
    assert(node == &gateway_node && !gateway_lease);
    if (gateway_acquire_error != ESP_OK) return gateway_acquire_error;
    gateway_attempt = (esp_openclaw_node_connect_attempt_t){0};
    *lease = gateway_lease = ++next_gateway_lease; return ESP_OK;
}
esp_err_t esp_openclaw_node_connect_lease_release(esp_openclaw_node_handle_t node, esp_openclaw_node_connect_lease_t lease)
{
    assert(node == &gateway_node && lease == gateway_lease);
    assert(gateway_attempt.id == 0 || gateway_attempt.state == ESP_OPENCLAW_NODE_CONNECT_ATTEMPT_SUCCEEDED ||
        gateway_attempt.state == ESP_OPENCLAW_NODE_CONNECT_ATTEMPT_FAILED);
    gateway_lease = 0; return ESP_OK;
}
esp_err_t esp_openclaw_node_connect_begin_authorized(esp_openclaw_node_handle_t node,
    esp_openclaw_node_connect_lease_t lease, const esp_openclaw_node_connect_request_t *request,
    esp_openclaw_node_connect_authorize_t authorize, void *context, esp_openclaw_node_connect_attempt_id_t *attempt)
{
    assert(node == &gateway_node && lease == gateway_lease && authorize != NULL && context != NULL);
    assert(request->source == ESP_OPENCLAW_NODE_CONNECT_SOURCE_SETUP_CODE && strlen(request->value) <= 4096);
    strcpy(gateway_code, request->value);
    gateway_authority = authorize; gateway_authority_context = context;
    gateway_attempt = (esp_openclaw_node_connect_attempt_t){.id = ++gateway_begins,
        .state = ESP_OPENCLAW_NODE_CONNECT_ATTEMPT_QUEUED};
    *attempt = gateway_attempt.id; return ESP_OK;
}
esp_err_t esp_openclaw_node_connect_snapshot(esp_openclaw_node_handle_t node,
    esp_openclaw_node_connect_lease_t lease, esp_openclaw_node_connect_attempt_id_t attempt,
    esp_openclaw_node_connect_attempt_t *result)
{ assert(node == &gateway_node && lease == gateway_lease && attempt == gateway_attempt.id); *result = gateway_attempt; return ESP_OK; }
esp_err_t esp_openclaw_node_connect_cancel(esp_openclaw_node_handle_t node,
    esp_openclaw_node_connect_lease_t lease, esp_openclaw_node_connect_attempt_id_t attempt)
{ assert(node == &gateway_node && lease == gateway_lease && attempt == gateway_attempt.id); gateway_attempt.cancel_requested = true; return ESP_OK; }

typedef struct {
    esp_openclaw_setup_session_t session;
    esp_openclaw_setup_wifi_t wifi;
    esp_openclaw_setup_gateway_t gateway;
    esp_openclaw_setup_transfer_t transfer;
    esp_openclaw_setup_dispatcher_t dispatcher;
    uint64_t generation;
    uint64_t peer, clock;
    uint32_t serial;
    bool live, probe_handoff, guarded;
    uint8_t reply[596];
} dispatcher_fixture_t;
static dispatcher_fixture_t wire;
static uint64_t wire_now(void *context) { (void)context; return wire.clock ? wire.clock : 100; }
static bool wire_peer(uint64_t peer, void *context)
{ return wire.guarded ? esp_openclaw_setup_security2_peer_is_current(peer) : peer == 7 && ((dispatcher_fixture_t *)context)->live; }
static void wire_cancel(esp_openclaw_setup_token_t token, void *context)
{ esp_openclaw_setup_dispatcher_cancel(&((dispatcher_fixture_t *)context)->dispatcher, token); }
static bool wire_hello(uint64_t generation, uint64_t peer, esp_openclaw_setup_bytes_t *label,
    esp_openclaw_setup_bytes_t *model, const uint8_t **challenge, void *context)
{
    static const uint8_t comparison[32] = {0xab};
    assert(generation == ((dispatcher_fixture_t *)context)->generation && peer == wire.peer);
    *label = (esp_openclaw_setup_bytes_t){(const uint8_t *)"Watch", 5};
    *model = (esp_openclaw_setup_bytes_t){(const uint8_t *)"S3", 2};
    *challenge = comparison; return true;
}
static void wire_open(void)
{
    wire.live = true;
    if (!wire.guarded) wire.peer = 7;
    assert(esp_openclaw_setup_open(&wire.session, &wire.generation) == ESP_OPENCLAW_SETUP_OK);
    assert(esp_openclaw_setup_attach(&wire.session, wire.generation, wire.peer) == ESP_OPENCLAW_SETUP_OK);
    assert(esp_openclaw_setup_transfer_open(&wire.transfer, wire.generation, wire.peer));
    wire.serial = 0;
}
static void wire_confirm(void)
{
    assert(esp_openclaw_setup_input_released(&wire.session, wire.generation) == ESP_OPENCLAW_SETUP_OK);
    assert(esp_openclaw_setup_confirm(&wire.session, wire.generation) == ESP_OPENCLAW_SETUP_OK);
    assert(esp_openclaw_setup_dispatcher_activate(&wire.dispatcher, wire.generation) == ESP_OK);
}
static void wire_integer(uint8_t *bytes, uint64_t value, unsigned count)
{ while (count) { bytes[--count] = value; value >>= 8; } }
static bool wire_exchange(unsigned operation, uint32_t id, const uint8_t *body, size_t body_size)
{
    uint8_t app[4114] = {1, operation}, input[464], output[464];
    wire_integer(app + 4, operation == 1 ? 0 : wire.generation, 8); wire_integer(app + 12, id, 4);
    if (body_size) memcpy(app + 16, body, body_size);
    uint16_t total = (uint16_t)(16 + body_size);
    esp_openclaw_setup_transfer_request_t handoff = {0};
    ++wire.serial;
    for (uint16_t offset = 0; offset < total;) {
        uint16_t count = total - offset; if (count > 448) count = 448;
        esp_openclaw_setup_frame_t frame = {.kind = ESP_OPENCLAW_SETUP_FRAME_REQUEST,
            .transfer_id = wire.serial, .total_size = total, .offset = offset, .payload_size = count, .payload = app + offset};
        size_t size, received;
        assert(esp_openclaw_setup_frame_encode(&frame, input, sizeof(input), &size));
        assert(esp_openclaw_setup_transfer_receive(&wire.transfer, wire.generation, 7,
            input, size, output, sizeof(output), &received, &handoff));
        offset += count;
    }
    assert(handoff.size == total);
    if (wire.probe_handoff) {
        uint8_t receipts = esp_openclaw_setup_snapshot(&wire.session).receipt_count;
        esp_openclaw_setup_transfer_request_t forged = handoff; ++forged.token.transfer_id;
        assert(!esp_openclaw_setup_dispatcher_handle(&wire.dispatcher, &forged));
        assert(esp_openclaw_setup_snapshot(&wire.session).receipt_count == receipts);
        forged = handoff; ++forged.data;
        assert(!esp_openclaw_setup_dispatcher_handle(&wire.dispatcher, &forged));
        assert(esp_openclaw_setup_snapshot(&wire.session).receipt_count == receipts);
        wire.probe_handoff = false;
    }
    if (!esp_openclaw_setup_dispatcher_handle(&wire.dispatcher, &handoff)) return false;
    // A second delivery of the same handoff cannot dispatch after publication.
    assert(!esp_openclaw_setup_dispatcher_handle(&wire.dispatcher, &handoff));
    for (unsigned offset = 0;;) {
        esp_openclaw_setup_frame_t pull = {.kind = ESP_OPENCLAW_SETUP_FRAME_PULL,
            .transfer_id = wire.serial, .offset = offset}, response;
        size_t size, received;
        assert(esp_openclaw_setup_frame_encode(&pull, input, sizeof(input), &size));
        assert(esp_openclaw_setup_transfer_receive(&wire.transfer, wire.generation, 7,
            input, size, output, sizeof(output), &received, &handoff));
        assert(esp_openclaw_setup_frame_decode(output, received, &response) && response.kind == ESP_OPENCLAW_SETUP_FRAME_RESPONSE);
        assert(response.total_size <= sizeof(wire.reply));
        memcpy(wire.reply + offset, response.payload, response.payload_size);
        offset += response.payload_size;
        if (offset == response.total_size) break;
    }
    return true;
}
static unsigned wire_code(void) { return (unsigned)wire.reply[2] * 256 + wire.reply[3]; }
static void revoke_scan(void) { wire.live = false; }

static void check_setup_dispatcher(void)
{
    assert(esp_openclaw_node_wifi_disconnect() == ESP_OK);
    esp_openclaw_setup_callbacks_t callbacks = {wire_now, wire_peer, wire_cancel, &wire};
    assert(esp_openclaw_setup_init(&wire.session, &callbacks));
    assert(esp_openclaw_setup_wifi_init(&wire.wifi, &wire.session));
    assert(esp_openclaw_setup_gateway_init(&wire.gateway, &wire.session, &gateway_node));
    assert(esp_openclaw_setup_transfer_init(&wire.transfer, &wire.session));
    assert(esp_openclaw_setup_dispatcher_init(&wire.dispatcher, &wire.session, &wire.transfer,
        &wire.wifi, &wire.gateway, wire_hello, &wire));
    wire_open();
    assert(wire_exchange(1, 0, NULL, 0) && wire_code() == 0 && wire.reply[16] == 1 && wire.reply[26] == 0xab);
    assert(wire_exchange(2, 0, NULL, 0) && wire_code() == 0x104);
    assert(wire_exchange(3, 0, NULL, 0) && wire_code() == 0x104);
    assert(esp_openclaw_setup_confirm(&wire.session, wire.generation) == ESP_OPENCLAW_SETUP_NEEDS_CONFIRMATION);
    wire_confirm();
    scan_found = 17;
    assert(wire_exchange(3, 0, NULL, 0) && wire_code() == 0);
    assert(wire.reply[16] == 16 && wire.reply[17] == 1 && wire.reply[19] == 17 && wire.reply[20] == 32);
    scan_found = 0;
    uint8_t credentials[100] = {0, 32}; memset(credentials + 2, 's', 32);
    credentials[34] = 0; credentials[35] = 64; memset(credentials + 36, 'f', 64);
    unsigned writes = config_writes;
    wire.probe_handoff = true;
    assert(wire_exchange(4, 1, credentials, sizeof(credentials)) && wire_code() == 1 && config_writes == writes);
    credentials[36] = '!';
    assert(wire_exchange(4, 1, credentials, sizeof(credentials)) && wire_code() == 2 && config_writes == writes);
    assert(wire_exchange(2, 1, NULL, 0) && wire_code() == 0);
#if SOC_WIFI_SUPPORTED && !CONFIG_ESP_WIFI_REMOTE_ENABLED && !CONFIG_ESP_HOST_WIFI_ENABLED
    assert(wire.reply[19] == 1); // New status transfer can read a pending apply.
    for (unsigned i = 0; i < 40 && esp_openclaw_setup_wifi_snapshot(&wire.wifi).attempt.state != ESP_OPENCLAW_NODE_WIFI_JOINING; ++i) {
        bool released; assert(esp_openclaw_setup_dispatcher_poll(&wire.dispatcher, &released) == ESP_OK); dispatch_all();
    }
    assert(memcmp(saved_config.sta.ssid, credentials + 2, 32) == 0 && saved_config.sta.password[0] == 'f');
    ip_event_got_ip_t ip = {.esp_netif = &station, .ip_info.ip.addr = 0x010200c0};
    wifi_handler(wifi_context, WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL);
    ip_handler(ip_context, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    bool released; assert(esp_openclaw_setup_dispatcher_poll(&wire.dispatcher, &released) == ESP_OK);
    assert(wire_exchange(2, 1, NULL, 0) && wire.reply[19] == 2 && wire.reply[26] == 1 && wire.reply[27] == 1);
#else
    assert(wire.reply[19] == 3 && wire.reply[25] == 2); // Truthful remote-radio unsupported receipt.
#endif
    esp_openclaw_setup_close(&wire.session);
    bool handed_back; assert(esp_openclaw_setup_dispatcher_poll(&wire.dispatcher, &handed_back) == ESP_OK && handed_back);
    assert(esp_openclaw_node_wifi_connect() == ESP_OK);
    ip_event_got_ip_t online = {.esp_netif = &station, .ip_info.ip.addr = 0x010200c0};
    ip_handler(ip_context, IP_EVENT, IP_EVENT_STA_GOT_IP, &online);
    wire_open(); wire_confirm();
    unsigned mutations = driver_mutations;
    assert(wire_exchange(3, 0, NULL, 0) && wire_code() == 0x105 && driver_mutations == mutations);
    uint8_t code[4098] = {16, 0}; memset(code + 2, 'x', 4096);
    assert(wire_exchange(5, 1, code, sizeof(code)) && wire_code() == 1 && strlen(gateway_code) == 4096);
    code[2] = 'y';
    unsigned began = gateway_begins;
    assert(wire_exchange(5, 1, code, sizeof(code)) && wire_code() == 2 && gateway_begins == began && gateway_code[0] == 'x');
    gateway_attempt.state = ESP_OPENCLAW_NODE_CONNECT_ATTEMPT_FAILED;
    gateway_attempt.local_err = ESP_ERR_TIMEOUT; gateway_attempt.credentials_saved = true;
    assert(esp_openclaw_setup_dispatcher_poll(&wire.dispatcher, &handed_back) == ESP_OK && !handed_back);
    assert(wire_exchange(2, 1, NULL, 0) && wire.reply[19] == 3 && wire.reply[25] == 4 && wire.reply[27] == 1);
    assert(wire_exchange(5, 2, code, sizeof(code)) && wire_code() == 1);
    assert(wire_exchange(2, 1, NULL, 0) && wire.reply[19] == 3 && wire.reply[25] == 4 &&
        wire.reply[26] == 0 && wire.reply[27] == 0 && wire.reply[28] == 0);
    assert(!wire_exchange(6, 0, NULL, 0) && gateway_attempt.cancel_requested);
    assert(esp_openclaw_setup_dispatcher_poll(&wire.dispatcher, &handed_back) == ESP_OK && !handed_back);
    assert(esp_openclaw_setup_snapshot(&wire.session).phase == ESP_OPENCLAW_SETUP_CLOSING);
    gateway_attempt.state = ESP_OPENCLAW_NODE_CONNECT_ATTEMPT_FAILED;
    gateway_attempt.failure_reason = ESP_OPENCLAW_NODE_CONNECT_FAILURE_CANCELED;
    assert(esp_openclaw_setup_dispatcher_poll(&wire.dispatcher, &handed_back) == ESP_OK && handed_back);
    assert(!esp_openclaw_setup_transfer_poll(&wire.transfer));
    // WiFi was polled before Gateway closed the session; its lease releases next tick.
    assert(esp_openclaw_setup_dispatcher_poll(&wire.dispatcher, &handed_back) == ESP_OK && !handed_back);
    assert(esp_openclaw_node_wifi_disconnect() == ESP_OK);
    wire_open(); wire_confirm();
    for (unsigned id = 1; id <= 32; ++id) assert(wire_exchange(5, id, code, sizeof(code)) && wire_code() == 1);
    assert(wire_exchange(5, 33, code, sizeof(code)) && wire_code() == 0x106);
    assert(wire_exchange(5, 1, code, sizeof(code)) && wire_code() == 2 && gateway_begins == began + 1);
    during_scan = revoke_scan;
    assert(!wire_exchange(3, 0, NULL, 0)); during_scan = NULL;
    assert(!esp_openclaw_setup_transfer_poll(&wire.transfer));
    assert(esp_openclaw_setup_dispatcher_poll(&wire.dispatcher, &handed_back) == ESP_OK && handed_back);
    puts("setup dispatcher: real framed admission/status, native WiFi copies, retries, saved observations, cancellation and revoke passed");
}

/* Public vendor boundary only; these opaque fixture bytes are NOT SRP/AES-GCM
 * proof. Actual guard/endpoint/frame/transfer/dispatcher/native WiFi code runs. */
void esp_log_level_set(const char *tag, int level) { (void)tag; assert(level == ESP_LOG_NONE); }
static bool quiet_broken;
int esp_log_level_get(const char *tag) { (void)tag; return quiet_broken ? 1 : ESP_LOG_NONE; }
static esp_err_t ble_sdk_init(protocomm_security_handle_t *handle) { *handle = &wire; return ESP_OK; }
static esp_err_t ble_sdk_cleanup(protocomm_security_handle_t handle) { assert(handle == &wire); return ESP_OK; }
static esp_err_t ble_sdk_session(protocomm_security_handle_t handle, uint32_t id)
{ assert(handle == &wire && id == 7); return ESP_OK; }
static esp_err_t ble_sdk_bytes(protocomm_security_handle_t handle, uint32_t id, const uint8_t *input,
    ssize_t size, uint8_t **output, ssize_t *length, bool encrypt)
{
    assert(handle == &wire && id == 7);
    *length = size + (encrypt ? 16 : -16); *output = calloc(1, (size_t)*length); assert(*output);
    memcpy(*output, input, encrypt ? (size_t)size : (size_t)*length); return ESP_OK;
}
static esp_err_t ble_sdk_encrypt(protocomm_security_handle_t h, uint32_t id, const uint8_t *b,
    ssize_t n, uint8_t **o, ssize_t *s) { return ble_sdk_bytes(h, id, b, n, o, s, true); }
static esp_err_t ble_sdk_decrypt(protocomm_security_handle_t h, uint32_t id, const uint8_t *b,
    ssize_t n, uint8_t **o, ssize_t *s) { return ble_sdk_bytes(h, id, b, n, o, s, false); }
const protocomm_security_t protocomm_security2 = {.ver = 2, .patch_ver = 1,
    .init = ble_sdk_init, .cleanup = ble_sdk_cleanup, .new_transport_session = ble_sdk_session,
    .close_transport_session = ble_sdk_session, .encrypt = ble_sdk_encrypt, .decrypt = ble_sdk_decrypt};
static bool mailbox_no_memory;
static uint8_t *mailbox_allocation;
static unsigned mailbox_discarded;
void *mailbox_test_malloc(size_t size)
{ assert(size == 464); mailbox_allocation = mailbox_no_memory ? NULL : malloc(size); return mailbox_allocation; }
void mailbox_test_free(void *bytes)
{
    assert(bytes == mailbox_allocation);
    for (size_t i = 0; i < 464; ++i) assert(mailbox_allocation[i] == 0);
    ++mailbox_discarded; free(bytes);
}

#if SOC_WIFI_SUPPORTED && !CONFIG_ESP_WIFI_REMOTE_ENABLED && !CONFIG_ESP_HOST_WIFI_ENABLED
static esp_openclaw_setup_mailbox_t mailbox;
static const protocomm_security_t *ble_security;
static protocomm_security_handle_t ble_handle;
static uint8_t ble_input[464], ble_reply[464];
static size_t ble_size;
static unsigned scan_mailbox_probes;
static pthread_mutex_t scan_gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t scan_condition = PTHREAD_COND_INITIALIZER;
static bool scan_entered, scan_release;

static esp_err_t ble_call(esp_openclaw_setup_frame_t *reply)
{
    uint8_t cipher[480] = {0}, *plain, *answer, *encrypted; ssize_t count, length, encrypted_size;
    memcpy(cipher, ble_input, ble_size);
    esp_err_t err = ble_security->decrypt(ble_handle, 7, cipher, (ssize_t)ble_size + 16, &plain, &count);
    if (err != ESP_OK) return err;
    protocomm_req_handler_t endpoint = esp_openclaw_setup_mailbox_endpoint;
    err = endpoint(7, plain, count, &answer, &length, &mailbox); free(plain);
    if (err != ESP_OK) return err;
    err = ble_security->encrypt(ble_handle, 7, answer, length, &encrypted, &encrypted_size); free(answer);
    if (err != ESP_OK) return err;
    memcpy(ble_reply, encrypted, (size_t)encrypted_size - 16); free(encrypted);
    assert(esp_openclaw_setup_frame_decode(ble_reply, (size_t)encrypted_size - 16, reply)); return ESP_OK;
}
static void ble_frame(esp_openclaw_setup_frame_kind_t kind, uint16_t total, uint16_t offset,
    const uint8_t *bytes, uint16_t count)
{
    esp_openclaw_setup_frame_t frame = {.kind = kind, .transfer_id = wire.serial,
        .total_size = total, .offset = offset, .payload_size = count, .payload = bytes};
    assert(esp_openclaw_setup_frame_encode(&frame, ble_input, sizeof(ble_input), &ble_size));
}
static void ble_pending(void)
{
    esp_openclaw_setup_frame_t reply; assert(ble_call(&reply) == ESP_OK);
    assert(reply.kind == ESP_OPENCLAW_SETUP_FRAME_ERROR && reply.payload[1] == ESP_OPENCLAW_SETUP_FRAME_PENDING);
}
static void ble_wiped(void)
{
    for (size_t i = 0; i < sizeof(mailbox.request); ++i) assert(!mailbox.request[i] && !mailbox.reply[i]);
}
static void ble_open(void)
{
    uint8_t opaque[17] = {0}, *plain; ssize_t size;
    assert(ble_security->new_transport_session(ble_handle, 7) == ESP_OK);
    assert(ble_security->decrypt(ble_handle, 7, opaque, sizeof(opaque), &plain, &size) == ESP_OK); free(plain);
    wire.peer = esp_openclaw_setup_security2_peer(); wire.clock = 100; wire_open();
    esp_openclaw_setup_snapshot_t snapshot = esp_openclaw_setup_snapshot(&wire.session);
    assert(esp_openclaw_setup_mailbox_open(&mailbox, wire.generation, wire.peer, snapshot.opened_ms));
}
static bool ble_request(unsigned operation, unsigned id, const uint8_t *body, size_t size)
{
    uint8_t app[128] = {1, operation}; assert(size + 16 <= sizeof(app));
    wire_integer(app + 4, operation == 1 ? 0 : wire.generation, 8); wire_integer(app + 12, id, 4);
    if (size) memcpy(app + 16, body, size);
    ++wire.serial; ble_frame(ESP_OPENCLAW_SETUP_FRAME_REQUEST, (uint16_t)(16 + size), 0, app, (uint16_t)(16 + size));
    ble_pending(); esp_openclaw_setup_mailbox_poll(&mailbox);
    esp_openclaw_setup_frame_t reply;
    if (ble_call(&reply) != ESP_OK) return false;
    assert(reply.kind == ESP_OPENCLAW_SETUP_FRAME_ACK); return true;
}
static esp_openclaw_setup_frame_t ble_pull(uint16_t offset)
{
    ble_frame(ESP_OPENCLAW_SETUP_FRAME_PULL, 0, offset, NULL, 0); ble_pending();
    esp_openclaw_setup_mailbox_poll(&mailbox);
    esp_openclaw_setup_frame_t reply; assert(ble_call(&reply) == ESP_OK && reply.kind == ESP_OPENCLAW_SETUP_FRAME_RESPONSE);
    return reply;
}
static void scan_probe_mailbox(void)
{
    pthread_mutex_lock(&scan_gate);
    ++scan_mailbox_probes;
    scan_entered = true; pthread_cond_signal(&scan_condition);
    while (!scan_release) pthread_cond_wait(&scan_condition, &scan_gate);
    pthread_mutex_unlock(&scan_gate);
}
static void *scan_owner(void *unused)
{ (void)unused; esp_openclaw_setup_mailbox_poll(&mailbox); return NULL; }
static void ble_during_scan(void)
{
    esp_openclaw_setup_frame_t reply;
    assert(ble_call(&reply) == ESP_OK && reply.kind == ESP_OPENCLAW_SETUP_FRAME_ACK);
    ble_frame(ESP_OPENCLAW_SETUP_FRAME_PULL, 0, 0, NULL, 0); ble_pending(); ble_pending();
    ble_frame(ESP_OPENCLAW_SETUP_FRAME_PULL, 0, 448, NULL, 0);
    assert(ble_call(&reply) == ESP_OK && reply.kind == ESP_OPENCLAW_SETUP_FRAME_ERROR && reply.payload[1] == ESP_OPENCLAW_SETUP_FRAME_BUSY);
    ble_frame(ESP_OPENCLAW_SETUP_FRAME_PULL, 0, 0, NULL, 0);
}
static void check_setup_mailbox(void)
{
    ble_security = esp_openclaw_setup_security2(); assert(ble_security && ble_security->init(&ble_handle) == ESP_OK);
    assert(esp_openclaw_setup_mailbox_init(&mailbox, &wire.transfer, &wire.dispatcher, wire_now, &wire));
    wire.guarded = true; wire.clock = 100;
    assert(esp_openclaw_setup_open(&wire.session, &wire.generation) == ESP_OPENCLAW_SETUP_OK);
    esp_openclaw_setup_snapshot_t snapshot = esp_openclaw_setup_snapshot(&wire.session);
    assert(esp_openclaw_setup_mailbox_open(&mailbox, wire.generation, 0, snapshot.opened_ms));
    uint8_t opaque[17] = {0}, *plain; ssize_t plain_size;
    assert(ble_security->new_transport_session(ble_handle, 7) == ESP_OK);
    assert(ble_security->decrypt(ble_handle, 7, opaque, sizeof(opaque), &plain, &plain_size) == ESP_OK); free(plain);
    wire.peer = esp_openclaw_setup_security2_peer(); wire.serial = 1;
    uint8_t first_hello[16] = {1, 1}; esp_openclaw_setup_frame_t reply;
    ble_frame(ESP_OPENCLAW_SETUP_FRAME_REQUEST, 16, 0, first_hello, 16); ble_pending(); ble_wiped();
    assert(esp_openclaw_setup_snapshot(&wire.session).phase == ESP_OPENCLAW_SETUP_DISCOVERABLE);
    uint8_t *denied = NULL; ssize_t denied_size = -1;
    assert(esp_openclaw_setup_mailbox_endpoint(8, ble_input, (ssize_t)ble_size, &denied, &denied_size, &mailbox) == ESP_ERR_INVALID_STATE);
    assert(!denied && denied_size == -1 && esp_openclaw_setup_security2_peer_is_current(wire.peer));
    assert(mailbox_discarded == 1); // Rejected SDK-owned reply allocation was wiped before release.
    assert(esp_openclaw_setup_attach(&wire.session, wire.generation, wire.peer) == ESP_OPENCLAW_SETUP_OK);
    assert(esp_openclaw_setup_transfer_open(&wire.transfer, wire.generation, wire.peer));
    assert(!esp_openclaw_setup_mailbox_open(&mailbox, wire.generation, wire.peer, snapshot.opened_ms + 1));
    assert(esp_openclaw_setup_mailbox_open(&mailbox, wire.generation, wire.peer, snapshot.opened_ms));
    assert(!esp_openclaw_setup_mailbox_open(&mailbox, wire.generation, wire.peer, snapshot.opened_ms));
    ble_pending(); esp_openclaw_setup_mailbox_poll(&mailbox); // Identical first hello, now canonical admission.
    assert(ble_call(&reply) == ESP_OK && reply.kind == ESP_OPENCLAW_SETUP_FRAME_ACK);
    reply = ble_pull(0); assert(reply.payload[16] == 1 && reply.payload[26] == 0xab);
    uint8_t hello[16] = {1, 1}; ++wire.serial;
    ble_frame(ESP_OPENCLAW_SETUP_FRAME_REQUEST, 16, 0, hello, 8);
    mailbox_no_memory = true;
    assert(ble_call(&reply) == ESP_ERR_NO_MEM); mailbox_no_memory = false;
    esp_openclaw_setup_mailbox_poll(&mailbox); // Allocation failure leaves prior delivered reply intact, no new admission.
    ble_pending();
    uint8_t original[464]; size_t original_size = ble_size; memcpy(original, ble_input, ble_size);
    ++wire.serial; ble_frame(ESP_OPENCLAW_SETUP_FRAME_REQUEST, 16, 0, hello, 8);
    assert(ble_call(&reply) == ESP_OK && reply.payload[1] == ESP_OPENCLAW_SETUP_FRAME_BUSY);
    --wire.serial; memcpy(ble_input, original, original_size); ble_size = original_size;
    memset(ble_input + 16, 0xee, 8); esp_openclaw_setup_mailbox_poll(&mailbox); // Owned copy survives SDK input retirement.
    ble_frame(ESP_OPENCLAW_SETUP_FRAME_REQUEST, 16, 8, hello + 8, 8);
    assert(ble_call(&reply) == ESP_OK && reply.payload[1] == ESP_OPENCLAW_SETUP_FRAME_BUSY); // Prior ACK not delivered.
    memcpy(ble_input, original, original_size); ble_size = original_size;
    assert(ble_call(&reply) == ESP_OK && reply.kind == ESP_OPENCLAW_SETUP_FRAME_ACK && reply.offset == 8);
    assert(ble_call(&reply) == ESP_OK && reply.offset == 8); // Lost ACK retry, no second handoff.
    ble_frame(ESP_OPENCLAW_SETUP_FRAME_PULL, 0, 0, NULL, 0); ble_pending();
    esp_openclaw_setup_mailbox_poll(&mailbox); ble_pending(); esp_openclaw_setup_mailbox_poll(&mailbox);
    // A premature canonical PENDING pull cannot trap the missing request fragment.
    ble_frame(ESP_OPENCLAW_SETUP_FRAME_REQUEST, 16, 8, hello + 8, 8); ble_pending();
    esp_openclaw_setup_mailbox_poll(&mailbox);
    assert(ble_call(&reply) == ESP_OK && reply.kind == ESP_OPENCLAW_SETUP_FRAME_ACK && reply.offset == 16);
    reply = ble_pull(0); assert(reply.payload[16] == 1 && reply.payload[26] == 0xab);
    wire_confirm();

    /* A real owner thread blocks at the native scan API while this thread drives
     * actual SDK endpoint callbacks: ACK/pull must not share the blocking lock. */
    scan_found = 17; during_scan = scan_probe_mailbox;
    uint8_t scan[16] = {1, 3}; wire_integer(scan + 4, wire.generation, 8);
    ++wire.serial; ble_frame(ESP_OPENCLAW_SETUP_FRAME_REQUEST, 16, 0, scan, 16); ble_pending();
    pthread_t owner; assert(pthread_create(&owner, NULL, scan_owner, NULL) == 0);
    pthread_mutex_lock(&scan_gate);
    while (!scan_entered) pthread_cond_wait(&scan_condition, &scan_gate);
    pthread_mutex_unlock(&scan_gate);
    ble_during_scan();
    pthread_mutex_lock(&scan_gate); scan_release = true; pthread_cond_signal(&scan_condition); pthread_mutex_unlock(&scan_gate);
    assert(pthread_join(owner, NULL) == 0); during_scan = NULL;
    assert(scan_mailbox_probes == 1); ble_pending(); esp_openclaw_setup_mailbox_poll(&mailbox);
    assert(ble_call(&reply) == ESP_OK && reply.kind == ESP_OPENCLAW_SETUP_FRAME_RESPONSE && reply.payload_size == 448 && reply.total_size == 596);
    uint8_t first[464]; size_t first_size = 16 + reply.payload_size; memcpy(first, ble_reply, first_size);
    assert(ble_call(&reply) == ESP_OK && memcmp(first, ble_reply, first_size) == 0); // Pull retries remain exact.
    reply = ble_pull(448); assert(reply.offset == 448 && reply.payload_size == 148); scan_found = 0;

    uint8_t credentials[16] = {0, 2, 'a', 'p', 0, 8, 'p', 'a', 's', 's', 'w', 'o', 'r', 'd'};
    unsigned writes = config_writes;
    assert(ble_request(4, 1, credentials, 14)); reply = ble_pull(0); assert(reply.payload[3] == 1);
    assert(ble_request(2, 1, NULL, 0)); reply = ble_pull(0); assert(reply.payload[19] == 1);
    uint64_t old_generation = wire.generation, old_peer = wire.peer;
    assert(!ble_request(6, 0, NULL, 0)); ble_wiped();
    for (unsigned i = 0; i < 40 && esp_openclaw_setup_snapshot(&wire.session).phase != ESP_OPENCLAW_SETUP_CLOSED; ++i) {
        bool released; assert(esp_openclaw_setup_dispatcher_poll(&wire.dispatcher, &released) == ESP_OK); dispatch_all();
    }
    bool handed_back; assert(esp_openclaw_setup_dispatcher_poll(&wire.dispatcher, &handed_back) == ESP_OK);
    assert(config_writes == writes && esp_openclaw_setup_snapshot(&wire.session).phase == ESP_OPENCLAW_SETUP_CLOSED);
    assert(ble_security->close_transport_session(ble_handle, 7) == ESP_OK); ble_open();
    assert(!esp_openclaw_setup_mailbox_close(&mailbox, old_generation, old_peer));
    assert(!esp_openclaw_setup_security2_revoke_peer(old_peer) && esp_openclaw_setup_security2_peer_is_current(wire.peer));
    assert(!esp_openclaw_setup_mailbox_open(&mailbox, old_generation, wire.peer, 100));
    assert(ble_request(1, 0, NULL, 0)); // New generation cannot return the old slot's reply.
    wire.clock = 100 + ESP_OPENCLAW_SETUP_TIMEOUT_MS - 1;
    assert(ble_call(&reply) == ESP_OK && reply.kind == ESP_OPENCLAW_SETUP_FRAME_ACK);
    wire.clock++; assert(ble_call(&reply) == ESP_ERR_INVALID_STATE); ble_wiped();
    assert(esp_openclaw_setup_snapshot(&wire.session).phase == ESP_OPENCLAW_SETUP_CLOSED);
    bool released; assert(esp_openclaw_setup_dispatcher_poll(&wire.dispatcher, &released) == ESP_OK);
    assert(ble_security->close_transport_session(ble_handle, 7) == ESP_OK); ble_open();
    assert(ble_request(1, 0, NULL, 0)); wire.clock = 99;
    assert(ble_call(&reply) == ESP_ERR_INVALID_STATE); ble_wiped();
    assert(esp_openclaw_setup_snapshot(&wire.session).phase == ESP_OPENCLAW_SETUP_CLOSED);
    assert(esp_openclaw_setup_dispatcher_poll(&wire.dispatcher, &released) == ESP_OK);
    assert(ble_security->close_transport_session(ble_handle, 7) == ESP_OK);
    wire.clock = 100;
    assert(esp_openclaw_setup_open(&wire.session, &wire.generation) == ESP_OPENCLAW_SETUP_OK);
    snapshot = esp_openclaw_setup_snapshot(&wire.session);
    assert(esp_openclaw_setup_mailbox_open(&mailbox, wire.generation, 0, snapshot.opened_ms));
    assert(ble_security->new_transport_session(ble_handle, 7) == ESP_OK);
    uint8_t *exit_plain = NULL; ssize_t exit_plain_size = 0;
    assert(ble_security->decrypt(ble_handle, 7, opaque, sizeof(opaque), &exit_plain, &exit_plain_size) == ESP_OK);
    assert(exit_plain != NULL && exit_plain_size == (ssize_t)sizeof(opaque) - 16);
    free(exit_plain);
    esp_openclaw_setup_security2_revoke(); // Server exit before owner bind.
    assert(esp_openclaw_setup_mailbox_close(&mailbox, wire.generation, 0));
    assert(!esp_openclaw_setup_mailbox_open(&mailbox, wire.generation, esp_openclaw_setup_security2_peer(), snapshot.opened_ms));
    assert(ble_call(&reply) == ESP_ERR_INVALID_STATE); ble_wiped(); esp_openclaw_setup_close(&wire.session);
    assert(ble_security->cleanup(ble_handle) == ESP_OK);
    puts("setup mailbox: actual SDK callbacks, owned fragments/pull retries, scan-time ACK, backpressure, cancel drain, generations and expiry passed");
}
#else
static void check_setup_mailbox(void) { assert(!esp_openclaw_setup_security2()); }
#endif

/* Drive the actual boot-lifetime task through FreeRTOS queue boundaries on a
 * host thread. A permit advances exactly one owner iteration; no runtime-only
 * tick helper or alternate state machine is compiled into production. */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t condition;
    uint8_t items[4][32];
    unsigned count, size, cycles;
    bool waiting, permit, executed;
    TickType_t wait;
} runtime_queue_t;
static runtime_queue_t *runtime_queue;
static bool fail_queue, fail_task;
static pthread_t runtime_thread;
static TaskFunction_t runtime_task;
static void *runtime_context;
QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t size)
{
    assert(length == 4 && size <= 32);
    if (fail_queue) return NULL;
    runtime_queue = calloc(1, sizeof(*runtime_queue)); assert(runtime_queue);
    assert(pthread_mutex_init(&runtime_queue->lock, NULL) == 0);
    assert(pthread_cond_init(&runtime_queue->condition, NULL) == 0);
    runtime_queue->size = size; return runtime_queue;
}
void vQueueDelete(QueueHandle_t value)
{
    runtime_queue_t *q = value; assert(q == runtime_queue && !q->executed);
    pthread_mutex_destroy(&q->lock); pthread_cond_destroy(&q->condition); free(q); runtime_queue = NULL;
}
BaseType_t xQueueSend(QueueHandle_t value, const void *item, TickType_t timeout)
{
    runtime_queue_t *q = value; assert(timeout == 0);
    pthread_mutex_lock(&q->lock);
    bool accepted = q->count < 4;
    if (accepted) memcpy(q->items[q->count++], item, q->size);
    pthread_mutex_unlock(&q->lock); return accepted;
}
BaseType_t xQueueReceive(QueueHandle_t value, void *item, TickType_t timeout)
{
    runtime_queue_t *q = value;
    pthread_mutex_lock(&q->lock);
    if (q->executed) ++q->cycles;
    q->executed = true; q->waiting = true; q->wait = timeout;
    pthread_cond_broadcast(&q->condition);
    while (!q->permit) pthread_cond_wait(&q->condition, &q->lock);
    q->permit = false; q->waiting = false;
    bool received = q->count != 0;
    if (received) { memcpy(item, q->items[0], q->size); memmove(q->items, q->items + 1, --q->count * sizeof(q->items[0])); }
    pthread_mutex_unlock(&q->lock); return received;
}
static void *run_runtime(void *unused) { (void)unused; runtime_task(runtime_context); return NULL; }
BaseType_t xTaskCreate(TaskFunction_t function, const char *name, uint32_t stack,
    void *context, UBaseType_t priority, TaskHandle_t *handle)
{
    (void)name; (void)stack; (void)priority; (void)handle;
    if (fail_task) return 0;
    runtime_task = function; runtime_context = context;
    assert(pthread_create(&runtime_thread, NULL, run_runtime, NULL) == 0);
    assert(pthread_detach(runtime_thread) == 0); return pdPASS;
}
#if SOC_WIFI_SUPPORTED && !CONFIG_ESP_WIFI_REMOTE_ENABLED && !CONFIG_ESP_HOST_WIFI_ENABLED && !CONFIG_BT_CTRL_MODEM_SLEEP
static void runtime_step(void)
{
    runtime_queue_t *q = runtime_queue;
    pthread_mutex_lock(&q->lock);
    while (!q->waiting) pthread_cond_wait(&q->condition, &q->lock);
    assert(q->count || q->wait == 20); /* Closed and drained must block indefinitely. */
    unsigned cycle = q->cycles;
    q->permit = true; pthread_cond_broadcast(&q->condition);
    while (q->cycles == cycle) pthread_cond_wait(&q->condition, &q->lock);
    pthread_mutex_unlock(&q->lock);
}
#endif

static char *runtime_salt, *runtime_verifier;
static unsigned runtime_wipes;
void runtime_test_free(void *data)
{
    size_t size = data && data == runtime_salt ? 16 : data && data == runtime_verifier ? 384 : 0;
    for (size_t i = 0; i < size; ++i) assert(((uint8_t *)data)[i] == 0);
    if (size) { ++runtime_wipes; if (data == runtime_salt) runtime_salt = NULL; else runtime_verifier = NULL; }
    free(data);
}

#if SOC_WIFI_SUPPORTED && !CONFIG_ESP_WIFI_REMOTE_ENABLED && !CONFIG_ESP_HOST_WIFI_ENABLED && !CONFIG_BT_CTRL_MODEM_SLEEP
struct protocomm {
    const protocomm_security_t *security;
    protocomm_security_handle_t handle;
    const protocomm_security2_params_t *parameters;
    protocomm_req_handler_t endpoint;
    void *context;
};
static protocomm_t *runtime_pc;
static unsigned construct_failure, runtime_stops, runtime_deletes, runtime_wakes;
static bool runtime_ble, cancel_in_srp, expire_in_srp, cancel_in_start, fail_stop;
static esp_openclaw_setup_runtime_handle_t runtime_handle;
static uint64_t runtime_request;
protocomm_t *protocomm_new(void)
{ assert(!runtime_pc); return construct_failure == 2 ? NULL : (runtime_pc = calloc(1, sizeof(*runtime_pc))); }
void protocomm_delete(protocomm_t *pc)
{
    assert(pc == runtime_pc && !runtime_ble);
    if (pc->security) {
        assert(pc->parameters->salt == runtime_salt && pc->parameters->verifier == runtime_verifier);
        assert(pc->security->cleanup(pc->handle) == ESP_OK);
    }
    ++runtime_deletes; free(pc); runtime_pc = NULL;
}
esp_err_t protocomm_set_security(protocomm_t *pc, const char *name, const protocomm_security_t *security, const void *params)
{
    assert(!strcmp(name, "prov-session") && security->ver == 2 && security->patch_ver == 1);
    if (construct_failure == 3) return ESP_FAIL;
    esp_err_t error = security->init(&pc->handle);
    if (error == ESP_OK) { pc->security = security; pc->parameters = params; }
    return error;
}
esp_err_t protocomm_set_version(protocomm_t *pc, const char *name, const char *version)
{
    assert(pc->security && !strcmp(name, "proto-ver"));
    cJSON *json = cJSON_Parse(version); assert(json);
    cJSON *prov = cJSON_GetObjectItemCaseSensitive(json, "prov");
    assert(cJSON_GetObjectItemCaseSensitive(prov, "sec_ver")->valueint == 2 &&
           cJSON_GetObjectItemCaseSensitive(prov, "sec_patch_ver")->valueint == 1);
    cJSON_Delete(json); return construct_failure == 4 ? ESP_FAIL : ESP_OK;
}
esp_err_t protocomm_add_endpoint(protocomm_t *pc, const char *name, protocomm_req_handler_t handler, void *context)
{
    assert(!strcmp(name, "oc-setup") && handler == esp_openclaw_setup_mailbox_endpoint);
    if (construct_failure == 5) return ESP_FAIL;
    pc->endpoint = handler; pc->context = context; return ESP_OK;
}
esp_err_t protocomm_ble_start(protocomm_t *pc, const protocomm_ble_config_t *config)
{
    assert(pc == runtime_pc && pc->endpoint && !runtime_ble && config->nu_lookup_count == 3);
    assert(!config->keep_ble_on && !config->ble_notify && !config->ble_bonding && !config->ble_link_encryption);
    if (construct_failure == 6) return ESP_FAIL;
    if (cancel_in_start) assert(esp_openclaw_setup_runtime_cancel(runtime_handle, runtime_request) == ESP_OK);
    runtime_ble = true; return ESP_OK;
}
esp_err_t protocomm_ble_stop(protocomm_t *pc)
{
    assert(pc == runtime_pc && runtime_ble && esp_openclaw_setup_security2_peer() == 0);
    assert(pthread_equal(pthread_self(), runtime_thread));
    ++runtime_stops;
    if (fail_stop) return ESP_FAIL;
    runtime_ble = false; return ESP_OK;
}
esp_err_t esp_srp_gen_salt_verifier(const char *username, int username_size, const char *password,
    int password_size, char **salt, int salt_size, char **verifier, int *verifier_size)
{
    assert(!strcmp(username, "openclaw") && username_size == 8 && password_size == 32 && strlen(password) == 32 && salt_size == 16);
    assert(!runtime_salt && !runtime_verifier && !runtime_ble && !quiet_broken);
    if (cancel_in_srp) assert(esp_openclaw_setup_runtime_cancel(runtime_handle, runtime_request) == ESP_OK);
    if (expire_in_srp) now_us += 120000000;
    if (construct_failure == 1) return ESP_FAIL;
    runtime_salt = *salt = malloc(16); runtime_verifier = *verifier = malloc(384);
    assert(*salt && *verifier); memset(*salt, 0x55, 16); memset(*verifier, 0x66, 384); *verifier_size = 384;
    return ESP_OK;
}
static void reconnect_runtime(void *context) { assert(context == &runtime_wakes); ++runtime_wakes; }
static esp_openclaw_setup_runtime_snapshot_t runtime_view(void)
{ esp_openclaw_setup_runtime_snapshot_t view; assert(esp_openclaw_setup_runtime_snapshot(runtime_handle, &view)); return view; }
static void runtime_open(void)
{
    uint64_t old = runtime_request;
    assert(esp_openclaw_setup_runtime_open(runtime_handle, &runtime_request) == ESP_OK && runtime_request > old);
    for (unsigned i = 0; i < 5 && runtime_view().phase == ESP_OPENCLAW_SETUP_RUNTIME_OPENING; ++i) runtime_step();
    assert(runtime_view().phase != ESP_OPENCLAW_SETUP_RUNTIME_OPENING);
}
static esp_openclaw_setup_frame_t runtime_frame(void)
{
    uint8_t cipher[480] = {0}, *plain, *output, *encrypted; ssize_t plain_size, size, encrypted_size;
    memcpy(cipher, ble_input, ble_size);
    assert(runtime_pc->security->decrypt(runtime_pc->handle, 7, cipher, ble_size + 16, &plain, &plain_size) == ESP_OK);
    assert(runtime_pc->endpoint(7, plain, plain_size, &output, &size, runtime_pc->context) == ESP_OK); free(plain);
    assert(runtime_pc->security->encrypt(runtime_pc->handle, 7, output, size, &encrypted, &encrypted_size) == ESP_OK); free(output);
    memcpy(ble_reply, encrypted, encrypted_size - 16); free(encrypted);
    esp_openclaw_setup_frame_t frame; assert(esp_openclaw_setup_frame_decode(ble_reply, encrypted_size - 16, &frame));
    return frame;
}
static esp_openclaw_setup_runtime_snapshot_t runtime_attach(void)
{
    assert(runtime_pc->security->new_transport_session(runtime_pc->handle, 7) == ESP_OK);
    /* Encode the real hello header rather than dispatching a helper request. */
    wire.serial = 1;
    uint8_t hello_bytes[16] = {1, 1}; ble_frame(ESP_OPENCLAW_SETUP_FRAME_REQUEST, 16, 0, hello_bytes, 16);
    assert(runtime_frame().payload[1] == ESP_OPENCLAW_SETUP_FRAME_PENDING);
    runtime_step(); /* First decrypt -> owner attach -> one-time mailbox bind. */
    assert(runtime_frame().payload[1] == ESP_OPENCLAW_SETUP_FRAME_PENDING);
    runtime_step(); assert(runtime_frame().kind == ESP_OPENCLAW_SETUP_FRAME_ACK);
    ble_frame(ESP_OPENCLAW_SETUP_FRAME_PULL, 0, 0, NULL, 0);
    assert(runtime_frame().payload[1] == ESP_OPENCLAW_SETUP_FRAME_PENDING);
    runtime_step(); esp_openclaw_setup_frame_t reply = runtime_frame();
    assert(reply.kind == ESP_OPENCLAW_SETUP_FRAME_RESPONSE && reply.payload_size > 32);
    esp_openclaw_setup_runtime_snapshot_t view = runtime_view();
    assert(view.peer && view.session.phase == ESP_OPENCLAW_SETUP_COMPARISON && strlen(view.comparison) == 9 && !view.qr[0]);
    const uint8_t *challenge = reply.payload + reply.payload_size - 32;
    char comparison[10]; snprintf(comparison, sizeof(comparison), "%02X%02X %02X%02X", challenge[0], challenge[1], challenge[2], challenge[3]);
    assert(!strcmp(view.comparison, comparison));
    return view;
}
static void runtime_request_frame(unsigned operation, const uint8_t *body, size_t body_size)
{
    uint8_t request[128] = {1, operation}; assert(body_size + 16 <= sizeof(request));
    wire_integer(request + 4, runtime_view().session.generation, 8); wire_integer(request + 12, 1, 4);
    if (body_size) memcpy(request + 16, body, body_size);
    ++wire.serial; ble_frame(ESP_OPENCLAW_SETUP_FRAME_REQUEST, body_size + 16, 0, request, body_size + 16);
    assert(runtime_frame().payload[1] == ESP_OPENCLAW_SETUP_FRAME_PENDING);
    runtime_step(); assert(runtime_frame().kind == ESP_OPENCLAW_SETUP_FRAME_ACK);
}
static void runtime_confirm(esp_openclaw_setup_runtime_snapshot_t view)
{
    assert(esp_openclaw_setup_runtime_action(runtime_handle, runtime_request, view.session.generation, view.peer, ESP_OPENCLAW_SETUP_LOCAL_NEUTRAL) == ESP_OK);
    assert(esp_openclaw_setup_runtime_action(runtime_handle, runtime_request, view.session.generation, view.peer, ESP_OPENCLAW_SETUP_LOCAL_CONFIRM) == ESP_OK);
    runtime_step(); runtime_step();
}
static void check_setup_runtime(void)
{
    esp_openclaw_setup_runtime_config_t config = {.node = &gateway_node,
        .label = "Fixture device", .model = "fixture-s3", .gateway_reconnect = reconnect_runtime, .context = &runtime_wakes};
    fail_queue = true; assert(esp_openclaw_setup_runtime_create(&config, &runtime_handle) == ESP_ERR_NO_MEM); fail_queue = false;
    fail_task = true; assert(esp_openclaw_setup_runtime_create(&config, &runtime_handle) == ESP_ERR_NO_MEM); fail_task = false;
    assert(esp_openclaw_setup_runtime_create(&config, &runtime_handle) == ESP_OK);
    assert(esp_openclaw_setup_runtime_create(&config, &runtime_handle) == ESP_ERR_INVALID_STATE);
    assert(esp_openclaw_setup_runtime_open(runtime_handle, &runtime_request) == ESP_OK);
    assert(esp_openclaw_setup_runtime_cancel(runtime_handle, runtime_request) == ESP_OK);
    runtime_step(); assert(runtime_view().can_open && !runtime_pc); /* Cancel queued OPEN before owner runs. */
    for (construct_failure = 1; construct_failure <= 6; ++construct_failure) {
        runtime_open();
        assert(runtime_view().can_open && runtime_view().error != ESP_OK && !runtime_pc && !runtime_salt && !runtime_verifier);
    }
    construct_failure = 0;
    cancel_in_srp = true; runtime_open(); cancel_in_srp = false; assert(runtime_view().can_open && !runtime_pc);
    expire_in_srp = true; runtime_open(); expire_in_srp = false; assert(runtime_view().can_open && !runtime_pc);
    cancel_in_start = true; runtime_open(); cancel_in_start = false; assert(runtime_view().can_open && !runtime_pc);
    quiet_broken = true; runtime_open(); quiet_broken = false; assert(runtime_view().can_open && !runtime_pc);
    runtime_open(); esp_openclaw_setup_runtime_snapshot_t discover = runtime_view();
    assert(runtime_ble && discover.qr[0] && strlen(discover.pop) == 32 && !discover.wifi.lease_held && !discover.gateway.lease_held);
    cJSON *qr = cJSON_Parse(discover.qr); assert(qr);
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(qr, "pop")->valuestring, discover.pop) &&
           !strcmp(cJSON_GetObjectItemCaseSensitive(qr, "username")->valuestring, "openclaw")); cJSON_Delete(qr);
    esp_openclaw_setup_runtime_snapshot_t view = runtime_attach();
    assert(esp_openclaw_setup_runtime_action(runtime_handle, runtime_request, view.session.generation, view.peer, ESP_OPENCLAW_SETUP_LOCAL_CONFIRM) == ESP_OK);
    runtime_step(); assert(runtime_view().session.phase == ESP_OPENCLAW_SETUP_COMPARISON);
    for (unsigned i = 0; i < 4; ++i) assert(esp_openclaw_setup_runtime_action(runtime_handle, runtime_request,
        view.session.generation, view.peer, ESP_OPENCLAW_SETUP_LOCAL_NEUTRAL) == ESP_OK);
    assert(esp_openclaw_setup_runtime_action(runtime_handle, runtime_request, view.session.generation, view.peer, ESP_OPENCLAW_SETUP_LOCAL_CONFIRM) == ESP_ERR_TIMEOUT);
    for (unsigned i = 0; i < 4; ++i) runtime_step();
    assert(esp_openclaw_setup_runtime_action(runtime_handle, runtime_request, view.session.generation, view.peer, ESP_OPENCLAW_SETUP_LOCAL_CONFIRM) == ESP_OK);
    gateway_acquire_error = ESP_FAIL;
    runtime_step(); view = runtime_view(); assert(view.session.phase == ESP_OPENCLAW_SETUP_READY && view.wifi.lease_held && !view.gateway.lease_held && view.error == ESP_FAIL);
    gateway_acquire_error = ESP_OK;
    for (unsigned i = 0; i < 3; ++i) assert(esp_openclaw_setup_runtime_action(runtime_handle, runtime_request,
        view.session.generation, view.peer, ESP_OPENCLAW_SETUP_LOCAL_RETRY) == ESP_OK);
    runtime_step(); view = runtime_view(); assert(view.gateway.lease_held && view.error == ESP_OK);
    unsigned deleted = runtime_deletes, stopped = runtime_stops;
    fail_stop = true; assert(esp_openclaw_setup_runtime_cancel(runtime_handle, runtime_request) == ESP_OK);
    assert(!runtime_view().qr[0] && esp_openclaw_setup_security2_peer() == 0);
    runtime_step(); view = runtime_view(); assert(view.phase == ESP_OPENCLAW_SETUP_RUNTIME_ERROR && !view.can_open);
    runtime_step(); /* A queued activation retry must not become a failed-stop cleanup retry. */
    assert(runtime_stops == stopped + 1 && runtime_deletes == deleted && runtime_salt && runtime_verifier && runtime_wakes);
    fail_stop = false;
    assert(esp_openclaw_setup_runtime_action(runtime_handle, runtime_request, view.session.generation, view.peer, ESP_OPENCLAW_SETUP_LOCAL_RETRY) == ESP_OK);
    runtime_step(); assert(runtime_view().can_open && !runtime_pc && runtime_deletes == deleted + 1);
    uint64_t stale = runtime_request;
    runtime_open(); assert(strcmp(discover.pop, runtime_view().pop));
    assert(esp_openclaw_setup_runtime_cancel(runtime_handle, stale) == ESP_ERR_INVALID_STATE && runtime_ble);
    now_us += 120000000; assert(!runtime_view().qr[0]); runtime_step(); assert(runtime_view().can_open && !runtime_ble);
    runtime_open(); view = runtime_attach(); runtime_confirm(view);
    uint8_t credentials[] = {0, 2, 'a', 'p', 0, 8, 'p', 'a', 's', 's', 'w', 'o', 'r', 'd'};
    runtime_request_frame(4, credentials, sizeof(credentials));
    for (unsigned i = 0; i < 40 && runtime_view().wifi.attempt.state != ESP_OPENCLAW_NODE_WIFI_JOINING; ++i) { dispatch_all(); runtime_step(); }
    assert(runtime_view().wifi.attempt.state == ESP_OPENCLAW_NODE_WIFI_JOINING);
    unsigned writes = config_writes;
    stop_result = ESP_FAIL; fail_stop = true;
    assert(esp_openclaw_setup_runtime_cancel(runtime_handle, runtime_request) == ESP_OK); runtime_step();
    assert(runtime_pc && !runtime_view().can_open && runtime_view().wifi.operation_pending &&
        runtime_view().wifi.attempt.state == ESP_OPENCLAW_NODE_WIFI_RECOVERY_REQUIRED);
    stop_result = ESP_OK; view = runtime_view();
    assert(esp_openclaw_setup_runtime_action(runtime_handle, runtime_request, view.session.generation, view.peer, ESP_OPENCLAW_SETUP_LOCAL_RETRY) == ESP_OK);
    runtime_step(); view = runtime_view();
    assert(runtime_pc && view.error != ESP_OK && view.wifi.attempt.state != ESP_OPENCLAW_NODE_WIFI_RECOVERY_REQUIRED);
    for (unsigned i = 0; i < 40 && runtime_view().wifi.lease_held; ++i) { runtime_step(); dispatch_all(); }
    view = runtime_view(); assert(runtime_pc && view.error != ESP_OK && !view.can_open && !view.wifi.lease_held);
    fail_stop = false;
    assert(esp_openclaw_setup_runtime_action(runtime_handle, runtime_request, view.session.generation, view.peer, ESP_OPENCLAW_SETUP_LOCAL_RETRY) == ESP_OK);
    for (unsigned i = 0; i < 40 && !runtime_view().can_open; ++i) { runtime_step(); dispatch_all(); }
    assert(runtime_view().can_open && config_writes == writes && !runtime_view().wifi.lease_held);
    runtime_open(); view = runtime_attach();
    assert(esp_openclaw_node_wifi_set_credentials("Fixture network", NULL) == ESP_OK);
    ip_event_got_ip_t ip = {.esp_netif = &station}; ip_handler(ip_context, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    runtime_confirm(view);
    uint8_t setup_code[] = {0, 4, 'c', 'o', 'd', 'e'};
    runtime_request_frame(5, setup_code, sizeof(setup_code));
    assert(runtime_view().gateway.operation_pending && gateway_authority(gateway_authority_context));
    assert(esp_openclaw_setup_runtime_cancel(runtime_handle, runtime_request) == ESP_OK && !gateway_authority(gateway_authority_context));
    runtime_step(); assert(!runtime_pc && !runtime_view().can_open && gateway_attempt.cancel_requested);
    uint64_t denied_request = 0;
    assert(esp_openclaw_setup_runtime_open(runtime_handle, &denied_request) == ESP_ERR_INVALID_STATE && !denied_request);
    gateway_attempt.state = ESP_OPENCLAW_NODE_CONNECT_ATTEMPT_FAILED;
    gateway_attempt.failure_reason = ESP_OPENCLAW_NODE_CONNECT_FAILURE_CANCELED;
    runtime_step(); runtime_step(); assert(runtime_view().can_open && !runtime_view().gateway.lease_held);
    assert(runtime_wipes >= 2 && runtime_queue->wait == portMAX_DELAY);
    puts("setup runtime: real owner task, SDK construction/unwind, first hello, local confirmation, cancel/stop retry, authority and idle passed");
}
#else
static void check_setup_runtime(void)
{
    esp_openclaw_setup_runtime_handle_t output = (void *)1; uint64_t request = 9;
    assert(esp_openclaw_setup_runtime_create(NULL, &output) == ESP_ERR_NOT_SUPPORTED && output == (void *)1);
    assert(esp_openclaw_setup_runtime_open(output, &request) == ESP_ERR_NOT_SUPPORTED && request == 9);
    assert(!runtime_queue);
}
#endif

int main(void)
{
    uint8_t seed[32] = {0x72}; unsigned random_before = random_calls;
    esp_err_t seed_error = esp_openclaw_node_wifi_setup_seed(seed);
    assert((seed_error == ESP_ERR_INVALID_STATE || seed_error == ESP_ERR_NOT_SUPPORTED) && seed[0] == 0x72 && random_calls == random_before);
    assert(esp_openclaw_node_register_device_commands(NULL, NULL) == ESP_OK);
    cJSON *result = status();
    check(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(result, "connected")), "uninitialized Wi-Fi is disconnected");
    check(!cJSON_HasObjectItem(result, "rssi"), "disconnected status omits AP observations");
    cJSON_Delete(result);

    assert(esp_openclaw_node_wifi_start() == ESP_OK);
#if SOC_WIFI_SUPPORTED && !CONFIG_ESP_WIFI_REMOTE_ENABLED && !CONFIG_ESP_HOST_WIFI_ENABLED
    unsigned mutations_before_seed = driver_mutations;
    seed_expected = true;
    assert(esp_openclaw_node_wifi_setup_seed(seed) == ESP_OK && random_calls == random_before + 1);
    seed_expected = false;
    assert(driver_mutations == mutations_before_seed);
    get_ps_result = ESP_FAIL;
    assert(esp_openclaw_node_wifi_setup_seed(seed) == ESP_FAIL && random_calls == random_before + 1);
    get_ps_result = ESP_OK; power_save = 1;
    assert(esp_openclaw_node_wifi_setup_seed(seed) == ESP_ERR_WIFI_STATE && random_calls == random_before + 1);
    power_save = WIFI_PS_NONE;
#else
    assert(esp_openclaw_node_wifi_setup_seed(seed) == ESP_ERR_NOT_SUPPORTED && random_calls == random_before && seed[0] == 0x72);
#endif
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
    check_setup_dispatcher();
    check_setup_mailbox();
    check_setup_runtime();
    printf("Wi-Fi status and setup: %u failed checks\n", failures);
    return failures ? 1 : 0;
}
