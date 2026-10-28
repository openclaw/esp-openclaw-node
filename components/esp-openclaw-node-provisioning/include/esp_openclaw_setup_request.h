#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_OPENCLAW_SETUP_REQUEST_HEADER_SIZE 16
#define ESP_OPENCLAW_SETUP_SSID_SIZE_MAX 32
#define ESP_OPENCLAW_SETUP_PASSWORD_SIZE_MAX 64
#define ESP_OPENCLAW_SETUP_CODE_SIZE_MAX 4096

typedef enum {
    ESP_OPENCLAW_SETUP_REQUEST_HELLO = 1,
    ESP_OPENCLAW_SETUP_REQUEST_STATUS = 2,
    ESP_OPENCLAW_SETUP_REQUEST_WIFI_SCAN = 3,
    ESP_OPENCLAW_SETUP_REQUEST_WIFI_APPLY = 4,
    ESP_OPENCLAW_SETUP_REQUEST_GATEWAY_APPLY = 5,
    ESP_OPENCLAW_SETUP_REQUEST_CANCEL = 6,
} esp_openclaw_setup_request_kind_t;

/* Local codec results, not application response codes on the wire. */
typedef enum {
    ESP_OPENCLAW_SETUP_REQUEST_OK,
    ESP_OPENCLAW_SETUP_REQUEST_INVALID,
    ESP_OPENCLAW_SETUP_REQUEST_UNSUPPORTED_VERSION,
    ESP_OPENCLAW_SETUP_REQUEST_UNKNOWN_OPERATION,
    ESP_OPENCLAW_SETUP_REQUEST_TOO_LARGE,
} esp_openclaw_setup_request_result_t;

typedef struct {
    const uint8_t *data;
    size_t size;
} esp_openclaw_setup_bytes_t;

typedef struct {
    esp_openclaw_setup_request_kind_t kind;
    uint64_t generation;
    uint32_t operation_id;
    union {
        struct {
            esp_openclaw_setup_bytes_t ssid;
            esp_openclaw_setup_bytes_t password;
        } wifi;
        esp_openclaw_setup_bytes_t setup_code;
    } payload;
} esp_openclaw_setup_request_t;

/* Structural decoding only: no authentication, confirmation or dispatch.
 * Spans borrow input and are not NUL-terminated. Copy to owned storage before
 * asynchronous use or adapting to C-string owners; wipe credential copies.
 * Input/output must not overlap. Failure leaves the output unchanged.
 * See docs/design/mobile-setup-requests.md for IDs and command semantics. */
esp_openclaw_setup_request_result_t esp_openclaw_setup_request_decode(
    const uint8_t *input, size_t size, esp_openclaw_setup_request_t *request);

#ifdef __cplusplus
}
#endif
