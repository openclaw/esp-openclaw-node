#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef int esp_err_t;
enum { ESP_OK, ESP_FAIL, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE, ESP_ERR_NO_MEM,
       ESP_ERR_WIFI_CONN, ESP_ERR_WIFI_NOT_CONNECT, ESP_ERR_WIFI_NOT_INIT, ESP_ERR_WIFI_STATE,
       ESP_ERR_NOT_SUPPORTED, ESP_ERR_TIMEOUT };
#ifndef SOC_WIFI_SUPPORTED
#define SOC_WIFI_SUPPORTED 1
#endif
#ifndef CONFIG_ESP_WIFI_REMOTE_ENABLED
#define CONFIG_ESP_WIFI_REMOTE_ENABLED 0
#endif
#ifndef CONFIG_ESP_HOST_WIFI_ENABLED
#define CONFIG_ESP_HOST_WIFI_ENABLED 0
#endif
const char *esp_err_to_name(esp_err_t);

#define ESP_RETURN_ON_ERROR(call, tag, ...) do { \
    esp_err_t result = (call); (void)(tag); if (result != ESP_OK) return result; \
} while (0)
#define ESP_LOGI(tag, ...) do { (void)(tag); if (false) printf(__VA_ARGS__); } while (0)
#define ESP_LOGW ESP_LOGI

typedef uint32_t TickType_t;
typedef uint32_t EventBits_t;
typedef void *EventGroupHandle_t;
typedef void *SemaphoreHandle_t;
#define portMAX_DELAY UINT32_MAX
#define pdFALSE 0
#define BIT0 1
EventGroupHandle_t xEventGroupCreate(void);
EventBits_t xEventGroupClearBits(EventGroupHandle_t, EventBits_t);
EventBits_t xEventGroupSetBits(EventGroupHandle_t, EventBits_t);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t, EventBits_t, int, int, TickType_t);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t, TickType_t);
int xSemaphoreGive(SemaphoreHandle_t);

typedef const char *esp_event_base_t;
#define ESP_EVENT_DEFINE_BASE(name) const char *name = #name
typedef void (*esp_event_handler_t)(void *, esp_event_base_t, int32_t, void *);
extern const char *WIFI_EVENT, *IP_EVENT;
enum { ESP_EVENT_ANY_ID = -1, WIFI_EVENT_STA_START, WIFI_EVENT_STA_DISCONNECTED,
       IP_EVENT_STA_GOT_IP, WIFI_EVENT_STA_CONNECTED };
esp_err_t esp_event_handler_register(esp_event_base_t, int32_t, esp_event_handler_t, void *);
esp_err_t esp_event_post(esp_event_base_t, int32_t, const void *, size_t, TickType_t);
typedef struct { int unused; } esp_netif_t;
esp_netif_t *esp_netif_create_default_wifi_sta(void);
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { esp_netif_t *esp_netif; struct { esp_ip4_addr_t ip, netmask, gw; } ip_info; } ip_event_got_ip_t;
typedef struct { uint8_t reason; } wifi_event_sta_disconnected_t;
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(p) ((p)->addr & 255U), (((p)->addr >> 8) & 255U), \
                 (((p)->addr >> 16) & 255U), (((p)->addr >> 24) & 255U)

typedef enum { WIFI_AUTH_OPEN, WIFI_AUTH_WEP, WIFI_AUTH_WPA_PSK, WIFI_AUTH_WPA2_PSK,
    WIFI_AUTH_WPA_WPA2_PSK, WIFI_AUTH_WPA2_ENTERPRISE, WIFI_AUTH_WPA3_PSK,
    WIFI_AUTH_WPA2_WPA3_PSK, WIFI_AUTH_WAPI_PSK } wifi_auth_mode_t;
typedef struct { int8_t rssi; uint8_t primary; wifi_auth_mode_t authmode; } wifi_ap_record_t;
typedef struct { struct {
    uint8_t ssid[32], password[64];
    struct { wifi_auth_mode_t authmode; } threshold;
    struct { bool capable, required; } pmf_cfg;
} sta; } wifi_config_t;
typedef struct { int unused; } wifi_init_config_t;
typedef struct {
    bool show_hidden;
    int scan_type;
    struct { struct { unsigned min, max; } active; } scan_time;
} wifi_scan_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
enum { WIFI_STORAGE_FLASH, WIFI_MODE_STA, WIFI_IF_STA, WIFI_PS_NONE, WIFI_SCAN_TYPE_ACTIVE };
esp_err_t esp_wifi_init(const wifi_init_config_t *);
esp_err_t esp_wifi_deinit(void);
esp_err_t esp_wifi_set_storage(int);
esp_err_t esp_wifi_set_mode(int);
esp_err_t esp_wifi_get_config(int, wifi_config_t *);
esp_err_t esp_wifi_set_config(int, const wifi_config_t *);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);
esp_err_t esp_wifi_restore(void);
esp_err_t esp_wifi_set_ps(int);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_disconnect(void);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *);
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *, bool);
esp_err_t esp_wifi_scan_get_ap_num(uint16_t *);
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *, wifi_ap_record_t *);
esp_err_t esp_wifi_clear_ap_list(void);

typedef void *esp_openclaw_node_handle_t;
typedef struct { const char *code, *message; } esp_openclaw_node_error_t;
typedef esp_err_t (*esp_openclaw_node_command_handler_t)(esp_openclaw_node_handle_t,
    void *, const char *, size_t, char **, esp_openclaw_node_error_t *);
typedef struct {
    const char *name;
    esp_openclaw_node_command_handler_t handler;
    void *context;
} esp_openclaw_node_command_t;
esp_err_t esp_openclaw_node_register_capability(esp_openclaw_node_handle_t, const char *);
esp_err_t esp_openclaw_node_register_command(esp_openclaw_node_handle_t, const esp_openclaw_node_command_t *);
typedef struct { char version[32], date[16], time[16]; } esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void);
const char *esp_get_idf_version(void);
int64_t esp_timer_get_time(void);
#define CONFIG_IDF_TARGET "host"
