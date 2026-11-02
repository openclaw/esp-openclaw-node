#pragma once

#include <stdbool.h>
#include "esp_openclaw_setup_request.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_OPENCLAW_SETUP_RESPONSE_HEADER_SIZE 16
#define ESP_OPENCLAW_SETUP_RESPONSE_SIZE_MAX 596
#define ESP_OPENCLAW_SETUP_RESPONSE_TEXT_MAX 64
#define ESP_OPENCLAW_SETUP_RESPONSE_CHALLENGE_SIZE 32
#define ESP_OPENCLAW_SETUP_RESPONSE_SCAN_MAX 16

/* These explicit values ARE the version 1 wire contract, not native results. */
typedef enum {
    ESP_OPENCLAW_SETUP_RESPONSE_OK = 0x0000,
    ESP_OPENCLAW_SETUP_RESPONSE_ACCEPTED = 0x0001,
    ESP_OPENCLAW_SETUP_RESPONSE_DUPLICATE = 0x0002,
    ESP_OPENCLAW_SETUP_RESPONSE_INVALID = 0x0100,
    ESP_OPENCLAW_SETUP_RESPONSE_VERSION = 0x0101,
    ESP_OPENCLAW_SETUP_RESPONSE_OPERATION = 0x0102,
    ESP_OPENCLAW_SETUP_RESPONSE_TOO_LARGE = 0x0103,
    ESP_OPENCLAW_SETUP_RESPONSE_CONFIRM = 0x0104,
    ESP_OPENCLAW_SETUP_RESPONSE_BUSY = 0x0105,
    ESP_OPENCLAW_SETUP_RESPONSE_FULL = 0x0106,
    ESP_OPENCLAW_SETUP_RESPONSE_NOT_FOUND = 0x0107,
    ESP_OPENCLAW_SETUP_RESPONSE_UNSUPPORTED = 0x0108,
    ESP_OPENCLAW_SETUP_RESPONSE_UNAVAILABLE = 0x0109,
    ESP_OPENCLAW_SETUP_RESPONSE_INTERNAL = 0x010a,
    ESP_OPENCLAW_SETUP_RESPONSE_STALE = 0x010b,
} esp_openclaw_setup_response_code_t;

typedef enum {
    ESP_OPENCLAW_SETUP_RESPONSE_COMPARISON = 1,
    ESP_OPENCLAW_SETUP_RESPONSE_READY = 2,
    ESP_OPENCLAW_SETUP_RESPONSE_WORKING = 3,
} esp_openclaw_setup_response_phase_t;

typedef enum {
    ESP_OPENCLAW_SETUP_RESPONSE_NO_RECEIPT = 0,
    ESP_OPENCLAW_SETUP_RESPONSE_PENDING = 1,
    ESP_OPENCLAW_SETUP_RESPONSE_SUCCEEDED = 2,
    ESP_OPENCLAW_SETUP_RESPONSE_FAILED = 3,
    ESP_OPENCLAW_SETUP_RESPONSE_UNKNOWN = 4,
} esp_openclaw_setup_response_outcome_t;

typedef enum {
    ESP_OPENCLAW_SETUP_RESPONSE_DETAIL_NONE = 0,
    ESP_OPENCLAW_SETUP_RESPONSE_DETAIL_INPUT = 1,
    ESP_OPENCLAW_SETUP_RESPONSE_DETAIL_UNSUPPORTED = 2,
    ESP_OPENCLAW_SETUP_RESPONSE_DETAIL_BUSY = 3,
    ESP_OPENCLAW_SETUP_RESPONSE_DETAIL_TIMEOUT = 4,
    ESP_OPENCLAW_SETUP_RESPONSE_DETAIL_CANCELLED = 5,
    ESP_OPENCLAW_SETUP_RESPONSE_DETAIL_RESOURCE = 6,
    ESP_OPENCLAW_SETUP_RESPONSE_DETAIL_FAILED = 7,
} esp_openclaw_setup_response_detail_t;

typedef enum {
    ESP_OPENCLAW_SETUP_RESPONSE_PROGRESS_UNKNOWN = 0,
    ESP_OPENCLAW_SETUP_RESPONSE_PREPARING = 1,
    ESP_OPENCLAW_SETUP_RESPONSE_CONNECTING = 2,
    ESP_OPENCLAW_SETUP_RESPONSE_DRAINING = 3,
    ESP_OPENCLAW_SETUP_RESPONSE_RECOVERY_REQUIRED = 4,
    ESP_OPENCLAW_SETUP_RESPONSE_TERMINAL = 5,
} esp_openclaw_setup_response_progress_t;

/* A positive save observation from this exact native attempt. False means no
 * confirmed save, never rollback or preservation of previous credentials. */
#define ESP_OPENCLAW_SETUP_RESPONSE_SAVE_CONFIRMED 0x01

typedef struct {
    esp_openclaw_setup_response_phase_t phase;
    uint8_t receipt_count;
    uint8_t operation; /* 0, request WIFI_APPLY (4), or GATEWAY_APPLY (5). */
    esp_openclaw_setup_response_outcome_t outcome;
    uint32_t operation_id;
    esp_openclaw_setup_response_detail_t detail;
    uint8_t effects_known, effects_value;
    esp_openclaw_setup_response_progress_t progress;
} esp_openclaw_setup_response_status_t;

typedef enum {
    ESP_OPENCLAW_SETUP_RESPONSE_NETWORK_OPEN = 0,
    ESP_OPENCLAW_SETUP_RESPONSE_NETWORK_PASSWORD = 1,
    ESP_OPENCLAW_SETUP_RESPONSE_NETWORK_UNSUPPORTED = 2,
} esp_openclaw_setup_response_security_t;

typedef struct {
    uint8_t ssid_size;
    uint8_t ssid[ESP_OPENCLAW_SETUP_SSID_SIZE_MAX];
    int8_t rssi;
    uint8_t channel;
    esp_openclaw_setup_response_security_t security;
} esp_openclaw_setup_response_network_t;

typedef struct {
    uint8_t operation; /* Request operation, or 0 for a decoding error. */
    esp_openclaw_setup_response_code_t code;
    uint64_t generation;
    uint32_t operation_id;
    union {
        struct {
            esp_openclaw_setup_response_phase_t phase;
            esp_openclaw_setup_bytes_t label, model;
            const uint8_t *challenge; /* Exactly CHALLENGE_SIZE peer-bound bytes. */
        } hello;
        esp_openclaw_setup_response_status_t status;
        struct {
            uint8_t count;
            uint16_t total;
            esp_openclaw_setup_response_network_t networks[ESP_OPENCLAW_SETUP_RESPONSE_SCAN_MAX];
        } scan;
    } body;
} esp_openclaw_setup_response_t;

/* Pure encoding only. No allocation, state, authority, native error mapping or
 * challenge generation. Spans borrow caller storage through this call. Input,
 * referenced spans and output must not overlap. Failure changes no output byte
 * or output_size. Cancel has no success reply: closing revokes reply authority.
 * See docs/design/mobile-setup-responses.md for exact fields and knowledge rules. */
bool esp_openclaw_setup_response_encode(const esp_openclaw_setup_response_t *response,
    uint8_t *output, size_t capacity, size_t *output_size);

#ifdef __cplusplus
}
#endif
