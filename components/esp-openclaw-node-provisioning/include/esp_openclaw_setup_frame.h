#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_OPENCLAW_SETUP_FRAME_HEADER_SIZE 16
#define ESP_OPENCLAW_SETUP_FRAME_PAYLOAD_MAX 448
#define ESP_OPENCLAW_SETUP_FRAME_SIZE_MAX 464
#define ESP_OPENCLAW_SETUP_MESSAGE_SIZE_MAX 8192

typedef enum {
    ESP_OPENCLAW_SETUP_FRAME_REQUEST = 1,
    ESP_OPENCLAW_SETUP_FRAME_PULL = 2,
    ESP_OPENCLAW_SETUP_FRAME_ACK = 0x81,
    ESP_OPENCLAW_SETUP_FRAME_RESPONSE = 0x82,
    ESP_OPENCLAW_SETUP_FRAME_ERROR = 0xff,
} esp_openclaw_setup_frame_kind_t;

typedef enum {
    ESP_OPENCLAW_SETUP_FRAME_INVALID = 1,
    ESP_OPENCLAW_SETUP_FRAME_BUSY = 2,
    ESP_OPENCLAW_SETUP_FRAME_STALE = 3,
    ESP_OPENCLAW_SETUP_FRAME_CONFLICT = 4,
    ESP_OPENCLAW_SETUP_FRAME_PENDING = 5,
    ESP_OPENCLAW_SETUP_FRAME_TOO_LARGE = 6,
    ESP_OPENCLAW_SETUP_FRAME_UNAUTHORIZED = 7,
    ESP_OPENCLAW_SETUP_FRAME_INTERNAL = 8,
} esp_openclaw_setup_frame_error_t;

typedef struct {
    esp_openclaw_setup_frame_kind_t kind;
    uint32_t transfer_id;
    uint16_t total_size;
    uint16_t offset;
    uint16_t payload_size;
    const uint8_t *payload;
} esp_openclaw_setup_frame_t;

/* Structural codec only: no authentication, reassembly or operation dispatch.
 * Decoded payload borrows input storage. Encode input/output must not overlap.
 * Outputs remain unchanged on failure. See docs/design/mobile-setup-frames.md. */
bool esp_openclaw_setup_frame_decode(const uint8_t *input, size_t size,
                                     esp_openclaw_setup_frame_t *frame);
bool esp_openclaw_setup_frame_encode(const esp_openclaw_setup_frame_t *frame,
                                     uint8_t *output, size_t capacity, size_t *size);

#ifdef __cplusplus
}
#endif
