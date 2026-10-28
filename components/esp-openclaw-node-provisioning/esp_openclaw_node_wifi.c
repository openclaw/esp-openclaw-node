/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_openclaw_node_wifi.h"

#include <stdbool.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "soc/soc_caps.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

static const char *TAG = "esp_openclaw_node_wifi";

static EventGroupHandle_t s_wifi_events;
static SemaphoreHandle_t s_wifi_lock;
static esp_openclaw_node_wifi_status_t s_status;
static bool s_started;
static bool s_driver_initialized;
static bool s_driver_running;
static esp_netif_t *s_station;
static bool s_should_reconnect;
static bool s_operation_in_progress;
static esp_openclaw_node_wifi_setup_lease_t s_setup_lease;
static esp_openclaw_node_wifi_setup_lease_t s_last_setup_lease;
static unsigned s_reconnect_calls;
static bool s_setup_retry_pending;

#define NATIVE_WIFI_SETUP (SOC_WIFI_SUPPORTED && !CONFIG_ESP_WIFI_REMOTE_ENABLED && !CONFIG_ESP_HOST_WIFI_ENABLED)

#if NATIVE_WIFI_SETUP
ESP_EVENT_DEFINE_BASE(OPENCLAW_WIFI_DRAIN);
typedef enum {
    ATTEMPT_QUEUED, ATTEMPT_STOP, ATTEMPT_DEINIT, ATTEMPT_POST_A, ATTEMPT_WAIT_A,
    ATTEMPT_POST_B, ATTEMPT_WAIT_B, ATTEMPT_RESTART, ATTEMPT_APPLY,
    ATTEMPT_CONNECT, ATTEMPT_WAIT_IP, ATTEMPT_BLOCKED, ATTEMPT_DONE,
} attempt_step_t;
typedef enum { ATTEMPT_JOIN, ATTEMPT_FAIL, ATTEMPT_CANCEL } attempt_target_t;
typedef struct { uint64_t attempt, round; } drain_cookie_t;
static struct {
    esp_openclaw_node_wifi_attempt_t result;
    esp_openclaw_node_wifi_setup_lease_t lease;
    wifi_config_t config;
    attempt_step_t step;
    attempt_target_t target;
    uint64_t round, since_ms;
    bool associated, got_ip, disconnected;
} s_attempt;
static uint64_t s_last_attempt;
#endif

#define WIFI_CONNECTED_BIT BIT0

static void clear_ip_fields_locked(void)
{
    s_status.connected = false;
    s_status.ap_info_valid = false;
    s_status.ip[0] = '\0';
    s_status.netmask[0] = '\0';
    s_status.gateway[0] = '\0';
    s_status.rssi = 0;
    s_status.channel = 0;
    s_status.authmode = WIFI_AUTH_OPEN;
}

static void clear_connection_state_locked(void)
{
    /* Publish the snapshot and waiter signal in the same critical section. */
    clear_ip_fields_locked();
    s_status.last_disconnect_reason = 0;
    if (s_wifi_events != NULL) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void update_credentials_status_locked(const char *ssid)
{
    size_t ssid_len = ssid != NULL ? strnlen(ssid, sizeof(s_status.ssid) - 1U) : 0;
    s_status.has_saved_network = ssid_len > 0;
    memset(s_status.ssid, 0, sizeof(s_status.ssid));
    if (s_status.has_saved_network) {
        memcpy(s_status.ssid, ssid, ssid_len);
    }
}

static void update_credentials_status_from_config_locked(const wifi_config_t *config)
{
    size_t ssid_len = strnlen((const char *)config->sta.ssid, sizeof(config->sta.ssid));
    s_status.has_saved_network = ssid_len > 0;
    s_status.ssid[0] = '\0';
    if (s_status.has_saved_network) {
        memcpy(s_status.ssid, config->sta.ssid, ssid_len);
        s_status.ssid[ssid_len] = '\0';
    }
}

static esp_err_t copy_wifi_config_field(
    uint8_t *dst,
    size_t dst_size,
    const char *value,
    const char *field_name)
{
    memset(dst, 0, dst_size);
    if (value == NULL) {
        return ESP_OK;
    }

    size_t value_len = strlen(value);
    if (value_len > dst_size) {
        ESP_LOGW(
            TAG,
            "%s length %u exceeds supported maximum %u bytes",
            field_name,
            (unsigned)value_len,
            (unsigned)dst_size);
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(dst, value, value_len);
    return ESP_OK;
}

static esp_err_t init_wifi_config(wifi_config_t *config, const char *ssid, const char *passphrase)
{
    memset(config, 0, sizeof(*config));
    ESP_RETURN_ON_ERROR(
        copy_wifi_config_field(config->sta.ssid, sizeof(config->sta.ssid), ssid, "SSID"),
        TAG,
        "invalid SSID");
    ESP_RETURN_ON_ERROR(
        copy_wifi_config_field(
            config->sta.password,
            sizeof(config->sta.password),
            passphrase,
            "passphrase"),
        TAG,
        "invalid passphrase");
    config->sta.threshold.authmode = WIFI_AUTH_OPEN;
    config->sta.pmf_cfg.capable = true;
    config->sta.pmf_cfg.required = false;
    return ESP_OK;
}

static esp_err_t begin_operation(void)
{
    if (!s_started || s_wifi_lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    bool busy = s_operation_in_progress || s_setup_lease != 0;
    if (!busy) s_operation_in_progress = true;
    xSemaphoreGive(s_wifi_lock);
    return busy ? ESP_ERR_WIFI_STATE : ESP_OK;
}

static void end_operation(void)
{
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    s_operation_in_progress = false;
    xSemaphoreGive(s_wifi_lock);
}

/* Reserve before dropping the lock so setup cannot take ownership between the
 * reconnect decision and its driver call. This does not drain an async join. */
static bool reserve_reconnect_locked(void)
{
    if (!s_status.has_saved_network || !s_should_reconnect) return false;
    if (s_setup_lease != 0) {
        s_setup_retry_pending = true;
        return false;
    }
    ++s_reconnect_calls;
    return true;
}

static esp_err_t run_reserved_reconnect(void)
{
    esp_err_t err = esp_wifi_connect();
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    --s_reconnect_calls;
    xSemaphoreGive(s_wifi_lock);
    return err;
}

esp_err_t esp_openclaw_node_wifi_setup_acquire(esp_openclaw_node_wifi_setup_lease_t *lease)
{
    if (lease == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_started || s_wifi_lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (s_setup_lease != 0 || s_operation_in_progress || s_reconnect_calls != 0) {
        err = ESP_ERR_WIFI_STATE;
    } else if (s_last_setup_lease == UINT64_MAX) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        s_setup_lease = ++s_last_setup_lease;
        s_setup_retry_pending = false;
        *lease = s_setup_lease;
    }
    xSemaphoreGive(s_wifi_lock);
    return err;
}

esp_err_t esp_openclaw_node_wifi_setup_release(
    esp_openclaw_node_wifi_setup_lease_t lease, esp_err_t *reconnect_result)
{
    if (lease == 0 || reconnect_result == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_started || s_wifi_lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    if (lease != s_setup_lease || s_operation_in_progress) {
        esp_err_t err = lease != s_setup_lease ? ESP_ERR_INVALID_STATE : ESP_ERR_WIFI_STATE;
        xSemaphoreGive(s_wifi_lock);
        return err;
    }
    bool retry = s_setup_retry_pending && !s_status.connected;
    s_setup_lease = 0;
    s_setup_retry_pending = false;
    retry = retry && reserve_reconnect_locked();
    xSemaphoreGive(s_wifi_lock);
    *reconnect_result = retry ? run_reserved_reconnect() : ESP_OK;
    return ESP_OK;
}

#if NATIVE_WIFI_SETUP
static uint64_t wifi_now_ms(void) { return (uint64_t)esp_timer_get_time() / 1000U; }

static void wipe_config(wifi_config_t *config)
{
    volatile unsigned char *bytes = (volatile unsigned char *)config;
    for (size_t i = 0; i < sizeof(*config); ++i) bytes[i] = 0;
}

static void wipe_attempt_config(void) { wipe_config(&s_attempt.config); }

static bool attempt_matches_locked(uint64_t lease, uint64_t id)
{
    return lease != 0 && lease == s_setup_lease && lease == s_attempt.lease &&
        id != 0 && id == s_attempt.result.id;
}

static void attempt_finish_locked(void)
{
    s_attempt.result.state = s_attempt.target == ATTEMPT_JOIN ? ESP_OPENCLAW_NODE_WIFI_SUCCEEDED :
        s_attempt.target == ATTEMPT_FAIL ? ESP_OPENCLAW_NODE_WIFI_FAILED : ESP_OPENCLAW_NODE_WIFI_CANCELLED;
    s_attempt.step = ATTEMPT_DONE;
    wipe_attempt_config();
    s_operation_in_progress = false;
}

static void attempt_block_locked(esp_err_t error)
{
    if (s_attempt.target == ATTEMPT_JOIN) {
        s_attempt.target = ATTEMPT_FAIL;
        s_attempt.result.error = error;
    }
    s_attempt.result.recovery_error = error;
    s_attempt.result.state = ESP_OPENCLAW_NODE_WIFI_RECOVERY_REQUIRED;
    s_attempt.step = ATTEMPT_BLOCKED;
    wipe_attempt_config();
}

static void attempt_drain_locked(void)
{
    if (s_attempt.round == UINT64_MAX) {
        attempt_block_locked(ESP_ERR_INVALID_STATE);
        return;
    }
    ++s_attempt.round;
    s_attempt.since_ms = wifi_now_ms();
    s_attempt.step = ATTEMPT_STOP;
    s_attempt.result.state = s_attempt.target == ATTEMPT_JOIN ?
        ESP_OPENCLAW_NODE_WIFI_PREPARING : ESP_OPENCLAW_NODE_WIFI_DRAINING;
    s_should_reconnect = false;
    s_setup_retry_pending = false;
    clear_connection_state_locked();
}

static void attempt_cancel_locked(void)
{
    if (s_attempt.step == ATTEMPT_DONE) return;
    if (s_attempt.target == ATTEMPT_JOIN) s_attempt.target = ATTEMPT_CANCEL;
    wipe_attempt_config();
    if (s_attempt.step == ATTEMPT_QUEUED) {
        attempt_finish_locked();
    } else if (s_attempt.step != ATTEMPT_BLOCKED) {
        s_attempt.result.state = ESP_OPENCLAW_NODE_WIFI_DRAINING;
        if (s_attempt.step >= ATTEMPT_APPLY && s_attempt.step <= ATTEMPT_WAIT_IP)
            attempt_drain_locked();
    }
}

static void wifi_drain_handler(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    (void)arg; (void)base;
    const drain_cookie_t *cookie = data;
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    if (attempt_matches_locked(s_attempt.lease, cookie->attempt) && cookie->round == s_attempt.round) {
        if (event_id == 0 && s_attempt.step == ATTEMPT_WAIT_A) s_attempt.step = ATTEMPT_POST_B;
        if (event_id == 1 && s_attempt.step == ATTEMPT_WAIT_B) s_attempt.step = ATTEMPT_RESTART;
    }
    xSemaphoreGive(s_wifi_lock);
}

static esp_err_t restart_station(void)
{
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&config);
    if (err != ESP_OK) return err;
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    s_driver_initialized = true;
    xSemaphoreGive(s_wifi_lock);
    if ((err = esp_wifi_set_storage(WIFI_STORAGE_FLASH)) != ESP_OK) return err;
    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) return err;
    if ((err = esp_wifi_start()) != ESP_OK) return err;
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    s_driver_running = true;
    xSemaphoreGive(s_wifi_lock);
    return esp_wifi_set_ps(WIFI_PS_NONE);
}
#endif

esp_err_t esp_openclaw_node_wifi_setup_join_begin(
    esp_openclaw_node_wifi_setup_lease_t lease, const char *ssid,
    const char *passphrase, uint64_t *attempt_id)
{
#if NATIVE_WIFI_SETUP
    if (lease == 0 || ssid == NULL || ssid[0] == '\0' || attempt_id == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_started || s_wifi_lock == NULL) return ESP_ERR_INVALID_STATE;
    wifi_config_t config;
    esp_err_t err = init_wifi_config(&config, ssid, passphrase);
    if (err != ESP_OK) {
        wipe_config(&config);
        return err;
    }
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    if (lease != s_setup_lease || s_operation_in_progress || s_last_attempt == UINT64_MAX) {
        err = lease == s_setup_lease && s_operation_in_progress ? ESP_ERR_WIFI_STATE : ESP_ERR_INVALID_STATE;
    } else {
        wipe_attempt_config();
        memset(&s_attempt, 0, sizeof(s_attempt));
        s_attempt.lease = lease;
        s_attempt.result.id = ++s_last_attempt;
        s_attempt.result.state = ESP_OPENCLAW_NODE_WIFI_PREPARING;
        s_attempt.config = config;
        s_attempt.step = ATTEMPT_QUEUED;
        s_operation_in_progress = true;
        *attempt_id = s_attempt.result.id;
    }
    xSemaphoreGive(s_wifi_lock);
    wipe_config(&config);
    return err;
#else
    (void)lease; (void)ssid; (void)passphrase; (void)attempt_id;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t esp_openclaw_node_wifi_setup_join_cancel(uint64_t lease, uint64_t id)
{
#if NATIVE_WIFI_SETUP
    if (!s_started || s_wifi_lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    bool matches = attempt_matches_locked(lease, id);
    if (matches) attempt_cancel_locked();
    xSemaphoreGive(s_wifi_lock);
    return matches ? ESP_OK : ESP_ERR_INVALID_STATE;
#else
    (void)lease; (void)id;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t esp_openclaw_node_wifi_setup_join_retry_drain(uint64_t lease, uint64_t id)
{
#if NATIVE_WIFI_SETUP
    if (!s_started || s_wifi_lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    bool retry = attempt_matches_locked(lease, id) && s_attempt.step == ATTEMPT_BLOCKED;
    if (retry) {
        s_attempt.result.recovery_error = ESP_OK;
        attempt_drain_locked();
    }
    xSemaphoreGive(s_wifi_lock);
    return retry ? ESP_OK : ESP_ERR_INVALID_STATE;
#else
    (void)lease; (void)id;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t esp_openclaw_node_wifi_setup_join_poll(
    uint64_t lease, uint64_t id, esp_openclaw_node_wifi_authorize_t authorize,
    void *context, esp_openclaw_node_wifi_attempt_t *result)
{
#if NATIVE_WIFI_SETUP
    if (authorize == NULL || result == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_started || s_wifi_lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    bool matches = attempt_matches_locked(lease, id);
    xSemaphoreGive(s_wifi_lock);
    if (!matches) return ESP_ERR_INVALID_STATE;
    bool allowed = authorize(context);
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    if (!attempt_matches_locked(lease, id)) {
        xSemaphoreGive(s_wifi_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!allowed) attempt_cancel_locked();
    if (s_attempt.step == ATTEMPT_WAIT_IP) {
        if (s_attempt.disconnected || wifi_now_ms() - s_attempt.since_ms >= ESP_OPENCLAW_NODE_WIFI_ATTEMPT_TIMEOUT_MS) {
            s_attempt.target = ATTEMPT_FAIL;
            s_attempt.result.error = s_attempt.disconnected ? ESP_ERR_WIFI_CONN : ESP_ERR_TIMEOUT;
            attempt_drain_locked();
        } else if (s_attempt.associated && s_attempt.got_ip && s_status.connected) {
            attempt_finish_locked();
        }
    } else if ((s_attempt.step == ATTEMPT_WAIT_A || s_attempt.step == ATTEMPT_WAIT_B) &&
               wifi_now_ms() - s_attempt.since_ms >= ESP_OPENCLAW_NODE_WIFI_ATTEMPT_TIMEOUT_MS) {
        attempt_block_locked(ESP_ERR_TIMEOUT);
    }
    attempt_step_t step = s_attempt.step;
    bool driver_running = s_driver_running;
    bool driver_initialized = s_driver_initialized;
    drain_cookie_t cookie = {.attempt = id, .round = s_attempt.round};
    if (step == ATTEMPT_POST_A) s_attempt.step = ATTEMPT_WAIT_A;
    if (step == ATTEMPT_POST_B) s_attempt.step = ATTEMPT_WAIT_B;
    xSemaphoreGive(s_wifi_lock);

    esp_err_t err = ESP_OK;
    switch (step) {
    case ATTEMPT_QUEUED:
        xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
        attempt_drain_locked();
        xSemaphoreGive(s_wifi_lock);
        break;
    case ATTEMPT_STOP:
        if (driver_running) err = esp_wifi_stop();
        break;
    case ATTEMPT_DEINIT:
        if (driver_initialized) err = esp_wifi_deinit();
        break;
    case ATTEMPT_POST_A:
    case ATTEMPT_POST_B:
        err = esp_event_post(OPENCLAW_WIFI_DRAIN, step == ATTEMPT_POST_A ? 0 : 1,
                             &cookie, sizeof(cookie), 0);
        break;
    case ATTEMPT_RESTART:
        err = restart_station();
        break;
    case ATTEMPT_APPLY:
        /* Driver reinitialization may block; check authority again at the write. */
        if (!authorize(context)) {
            xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
            attempt_cancel_locked();
            xSemaphoreGive(s_wifi_lock);
            break;
        }
        err = esp_wifi_set_config(WIFI_IF_STA, &s_attempt.config);
        break;
    case ATTEMPT_CONNECT:
        if (!authorize(context)) {
            xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
            attempt_cancel_locked();
            xSemaphoreGive(s_wifi_lock);
            break;
        }
        xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
        s_attempt.step = ATTEMPT_WAIT_IP;
        s_attempt.result.state = ESP_OPENCLAW_NODE_WIFI_JOINING;
        s_attempt.since_ms = wifi_now_ms();
        s_attempt.associated = s_attempt.got_ip = s_attempt.disconnected = false;
        xSemaphoreGive(s_wifi_lock);
        err = esp_wifi_connect();
        break;
    default:
        break;
    }
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    if (!attempt_matches_locked(lease, id)) {
        xSemaphoreGive(s_wifi_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (err != ESP_OK && cookie.round == s_attempt.round) {
        if (step == ATTEMPT_APPLY || step == ATTEMPT_CONNECT) {
            s_attempt.target = ATTEMPT_FAIL;
            s_attempt.result.error = err;
            wipe_attempt_config();
            attempt_drain_locked();
        } else {
            attempt_block_locked(err);
        }
    } else if (cookie.round == s_attempt.round && s_attempt.step == step) {
        if (step == ATTEMPT_STOP) {
            s_driver_running = false;
            s_attempt.step = ATTEMPT_DEINIT;
        }
        if (step == ATTEMPT_DEINIT) {
            s_driver_initialized = false;
            s_attempt.step = ATTEMPT_POST_A;
        }
        if (step == ATTEMPT_RESTART) {
            if (s_attempt.target != ATTEMPT_JOIN) attempt_finish_locked();
            else s_attempt.step = ATTEMPT_APPLY;
        }
        if (step == ATTEMPT_APPLY) {
            update_credentials_status_from_config_locked(&s_attempt.config);
            s_should_reconnect = true;
            s_attempt.result.credentials_saved = true;
            wipe_attempt_config();
            s_attempt.step = ATTEMPT_CONNECT;
        }
    }
    *result = s_attempt.result;
    xSemaphoreGive(s_wifi_lock);
    return ESP_OK;
#else
    (void)lease; (void)id; (void)authorize; (void)context; (void)result;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
#if NATIVE_WIFI_SETUP
        case WIFI_EVENT_STA_CONNECTED:
            xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
            if (s_attempt.step == ATTEMPT_WAIT_IP && !s_attempt.disconnected)
                s_attempt.associated = true;
            xSemaphoreGive(s_wifi_lock);
            break;
#endif
        case WIFI_EVENT_STA_START:
            if (s_wifi_lock != NULL) {
                xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
                bool should_connect = reserve_reconnect_locked();
                char ssid[sizeof(s_status.ssid)] = {0};
                strncpy(ssid, s_status.ssid, sizeof(ssid) - 1U);
                xSemaphoreGive(s_wifi_lock);
                if (should_connect) {
                    ESP_LOGI(TAG, "connecting to SSID %s", ssid);
                    if (run_reserved_reconnect() != ESP_OK) {
                        ESP_LOGW(TAG, "initial wifi connect failed");
                    }
                } else {
                    ESP_LOGI(TAG, "wifi ready; waiting for credentials from REPL");
                }
            }
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *event =
                (const wifi_event_sta_disconnected_t *)event_data;
            bool should_retry = false;
            if (s_wifi_lock != NULL) {
                xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
                clear_connection_state_locked();
                s_status.last_disconnect_reason = (uint8_t)event->reason;
#if NATIVE_WIFI_SETUP
                if (s_attempt.step == ATTEMPT_WAIT_IP) {
                    s_attempt.disconnected = true;
                    s_attempt.result.disconnect_reason = (uint8_t)event->reason;
                }
#endif
                should_retry = reserve_reconnect_locked();
                xSemaphoreGive(s_wifi_lock);
            }
            if (should_retry) {
                ESP_LOGW(TAG, "wifi disconnected, reason=%u; reconnecting", (unsigned)event->reason);
                esp_err_t err = run_reserved_reconnect();
                if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
                    ESP_LOGW(TAG, "wifi reconnect failed: %s", esp_err_to_name(err));
                }
            } else {
                ESP_LOGI(TAG, "wifi disconnected, reason=%u", (unsigned)event->reason);
            }
            break;
        }
        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        if (event->esp_netif != s_station) return;
        if (s_wifi_lock != NULL) {
            xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
            /* A queued IP event cannot undo an explicit disconnect or forget. */
            if (!s_should_reconnect || !s_status.has_saved_network) {
                xSemaphoreGive(s_wifi_lock);
                return;
            }
#if NATIVE_WIFI_SETUP
            if (s_attempt.step == ATTEMPT_WAIT_IP) {
                if (!s_attempt.associated || s_attempt.disconnected) {
                    xSemaphoreGive(s_wifi_lock);
                    return;
                }
                s_attempt.got_ip = true;
            }
#endif
            s_status.connected = true;
            s_status.last_disconnect_reason = 0;
            snprintf(s_status.ip, sizeof(s_status.ip), IPSTR, IP2STR(&event->ip_info.ip));
            snprintf(s_status.netmask, sizeof(s_status.netmask), IPSTR, IP2STR(&event->ip_info.netmask));
            snprintf(s_status.gateway, sizeof(s_status.gateway), IPSTR, IP2STR(&event->ip_info.gw));
            if (s_wifi_events != NULL) {
                xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
            }
            xSemaphoreGive(s_wifi_lock);
        }
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

esp_err_t esp_openclaw_node_wifi_start(void)
{
    if (s_started) {
        xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
        bool ready = s_driver_initialized && s_driver_running && !s_operation_in_progress;
        xSemaphoreGive(s_wifi_lock);
        return ready ? ESP_OK : ESP_ERR_WIFI_STATE;
    }

    s_wifi_events = xEventGroupCreate();
    s_wifi_lock = xSemaphoreCreateMutex();
    if (s_wifi_events == NULL || s_wifi_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memset(&s_status, 0, sizeof(s_status));

    s_station = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    ESP_RETURN_ON_ERROR(err, TAG, "esp_wifi_init failed");
    s_driver_initialized = true;
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL),
        TAG,
        "register WIFI_EVENT failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL),
        TAG,
        "register IP_EVENT failed");
#if NATIVE_WIFI_SETUP
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(OPENCLAW_WIFI_DRAIN, ESP_EVENT_ANY_ID, &wifi_drain_handler, NULL),
        TAG, "register Wi-Fi drain failed");
#endif

    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), TAG, "esp_wifi_set_storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "esp_wifi_set_mode failed");

    wifi_config_t wifi_config = {0};
    err = esp_wifi_get_config(WIFI_IF_STA, &wifi_config);
    ESP_RETURN_ON_ERROR(err, TAG, "esp_wifi_get_config failed");

    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    update_credentials_status_from_config_locked(&wifi_config);
    s_should_reconnect = s_status.has_saved_network;
    clear_ip_fields_locked();
    s_status.last_disconnect_reason = 0;
    xSemaphoreGive(s_wifi_lock);

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");
    s_driver_running = true;
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "esp_wifi_set_ps failed");

    s_started = true;
    return ESP_OK;
}

esp_err_t esp_openclaw_node_wifi_set_credentials(const char *ssid, const char *passphrase)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_started || s_wifi_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const char *resolved_passphrase = passphrase != NULL ? passphrase : "";
    wifi_config_t new_config = {0};
    ESP_RETURN_ON_ERROR(init_wifi_config(&new_config, ssid, resolved_passphrase), TAG, "invalid wifi config");

    esp_err_t err = begin_operation();
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_STA, &new_config);
    if (err != ESP_OK) goto done;

    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    clear_connection_state_locked();
    update_credentials_status_locked(ssid);
    s_should_reconnect = true;
    xSemaphoreGive(s_wifi_lock);

    err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT && err != ESP_ERR_WIFI_CONN) {
        goto done;
    }
    err = esp_wifi_connect();
    if (err == ESP_ERR_WIFI_CONN) {
        err = ESP_OK;
    }
done:
    end_operation();
    return err;
}

esp_err_t esp_openclaw_node_wifi_clear_credentials(void)
{
    if (!s_started || s_wifi_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = begin_operation();
    if (err != ESP_OK) return err;
    err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        goto done;
    }
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    s_driver_running = false;
    xSemaphoreGive(s_wifi_lock);

    err = esp_wifi_restore();
    if (err != ESP_OK) goto done;
    err = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (err != ESP_OK) goto done;
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) goto done;

    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    clear_connection_state_locked();
    update_credentials_status_locked(NULL);
    s_should_reconnect = false;
    xSemaphoreGive(s_wifi_lock);

    err = esp_wifi_start();
    if (err != ESP_OK) goto done;
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    s_driver_running = true;
    xSemaphoreGive(s_wifi_lock);
    err = esp_wifi_set_ps(WIFI_PS_NONE);
done:
    end_operation();
    return err;
}

esp_err_t esp_openclaw_node_wifi_connect(void)
{
    if (!s_started || s_wifi_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = begin_operation();
    if (err != ESP_OK) return err;
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    bool wifi_is_configured = s_status.has_saved_network;
    s_should_reconnect = wifi_is_configured;
    xSemaphoreGive(s_wifi_lock);
    if (!wifi_is_configured) {
        end_operation();
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_wifi_connect();
    end_operation();
    if (err == ESP_ERR_WIFI_CONN) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "starting wifi connect failed");
    return ESP_OK;
}

esp_err_t esp_openclaw_node_wifi_disconnect(void)
{
    if (!s_started || s_wifi_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = begin_operation();
    if (err != ESP_OK) return err;
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    s_should_reconnect = false;
    clear_connection_state_locked();
    xSemaphoreGive(s_wifi_lock);

    err = esp_wifi_disconnect();
    end_operation();
    if (err == ESP_ERR_WIFI_NOT_CONNECT) {
        err = ESP_OK;
    }
    return err;
}

static esp_err_t scan(
    esp_openclaw_node_wifi_setup_lease_t lease,
    wifi_ap_record_t *records, uint16_t *count, uint16_t *total)
{
    if (records == NULL || count == NULL || total == NULL ||
        *count == 0 || *count > ESP_OPENCLAW_NODE_WIFI_SCAN_LIMIT) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t capacity = *count;
    *count = 0;
    *total = 0;
    if (!s_started || s_wifi_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    if (lease != s_setup_lease) {
        xSemaphoreGive(s_wifi_lock);
        return lease == 0 ? ESP_ERR_WIFI_STATE : ESP_ERR_INVALID_STATE;
    }
    // Provisioning scans must not compete with a live connection or its retries.
    bool busy = s_status.connected || s_should_reconnect || s_operation_in_progress;
    if (!busy) s_operation_in_progress = true;
    xSemaphoreGive(s_wifi_lock);
    if (busy) return ESP_ERR_WIFI_STATE;

    const wifi_scan_config_t config = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {.min = 0, .max = 120},
    };
    esp_err_t err = esp_wifi_scan_start(&config, true);
    if (err == ESP_OK) {
        err = esp_wifi_scan_get_ap_num(total);
        if (err == ESP_OK && *total > 0) {
            uint16_t returned = capacity;
            err = esp_wifi_scan_get_ap_records(&returned, records);
            if (err == ESP_OK) *count = returned;
        }
        // get_ap_records drains the driver list; failures and empty scans don't.
        if (err != ESP_OK || *total == 0) esp_wifi_clear_ap_list();
    }

    end_operation();
    return err;
}

esp_err_t esp_openclaw_node_wifi_scan(wifi_ap_record_t *records, uint16_t *count, uint16_t *total)
{
    return scan(0, records, count, total);
}

esp_err_t esp_openclaw_node_wifi_setup_scan(
    esp_openclaw_node_wifi_setup_lease_t lease,
    wifi_ap_record_t *records, uint16_t *count, uint16_t *total)
{
    if (lease == 0) return ESP_ERR_INVALID_ARG;
    return scan(lease, records, count, total);
}

bool esp_openclaw_node_wifi_wait_for_connection(TickType_t timeout_ticks)
{
    if (s_wifi_events == NULL) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events,
        WIFI_CONNECTED_BIT,
        pdFALSE,
        pdFALSE,
        timeout_ticks);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

bool esp_openclaw_node_wifi_is_connected(void)
{
    if (s_wifi_lock == NULL) {
        return false;
    }

    bool connected = false;
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    connected = s_status.connected;
    xSemaphoreGive(s_wifi_lock);
    return connected;
}

void esp_openclaw_node_wifi_get_status(esp_openclaw_node_wifi_status_t *status)
{
    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));
    if (s_wifi_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    *status = s_status;
    xSemaphoreGive(s_wifi_lock);

    if (status->connected) {
        wifi_ap_record_t ap_info = {0};
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            status->ap_info_valid = true;
            status->rssi = ap_info.rssi;
            status->channel = ap_info.primary;
            status->authmode = (uint8_t)ap_info.authmode;
        }
    }
}
