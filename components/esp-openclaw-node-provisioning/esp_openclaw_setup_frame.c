#include "esp_openclaw_setup_frame.h"

#include <string.h>

static uint16_t read16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void write16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static bool valid(const esp_openclaw_setup_frame_t *frame)
{
    if (frame->transfer_id == 0 || frame->payload_size > ESP_OPENCLAW_SETUP_FRAME_PAYLOAD_MAX ||
        (frame->payload_size != 0 && frame->payload == NULL)) return false;

    switch (frame->kind) {
    case ESP_OPENCLAW_SETUP_FRAME_REQUEST:
    case ESP_OPENCLAW_SETUP_FRAME_RESPONSE:
        return frame->total_size != 0 && frame->total_size <= ESP_OPENCLAW_SETUP_MESSAGE_SIZE_MAX &&
            frame->payload_size != 0 && frame->offset < frame->total_size &&
            frame->payload_size <= frame->total_size - frame->offset;
    case ESP_OPENCLAW_SETUP_FRAME_PULL:
        return frame->total_size == 0 && frame->offset < ESP_OPENCLAW_SETUP_MESSAGE_SIZE_MAX &&
            frame->payload_size == 0;
    case ESP_OPENCLAW_SETUP_FRAME_ACK:
        return frame->total_size != 0 && frame->total_size <= ESP_OPENCLAW_SETUP_MESSAGE_SIZE_MAX &&
            frame->offset <= frame->total_size && frame->payload_size == 0;
    case ESP_OPENCLAW_SETUP_FRAME_ERROR:
        return frame->total_size == 0 && frame->offset == 0 && frame->payload_size == 2 &&
            read16(frame->payload) >= ESP_OPENCLAW_SETUP_FRAME_INVALID &&
            read16(frame->payload) <= ESP_OPENCLAW_SETUP_FRAME_INTERNAL;
    default:
        return false;
    }
}

bool esp_openclaw_setup_frame_decode(const uint8_t *input, size_t size,
                                     esp_openclaw_setup_frame_t *frame)
{
    if (input == NULL || frame == NULL || size < ESP_OPENCLAW_SETUP_FRAME_HEADER_SIZE ||
        size > ESP_OPENCLAW_SETUP_FRAME_SIZE_MAX || input[0] != 'O' || input[1] != 'C' ||
        input[2] != 1 || input[14] != 0 || input[15] != 0) return false;
    esp_openclaw_setup_frame_t decoded = {
        .kind = (esp_openclaw_setup_frame_kind_t)input[3],
        .transfer_id = read32(input + 4),
        .total_size = read16(input + 8),
        .offset = read16(input + 10),
        .payload_size = read16(input + 12),
        .payload = input + ESP_OPENCLAW_SETUP_FRAME_HEADER_SIZE,
    };
    if (decoded.payload_size != size - ESP_OPENCLAW_SETUP_FRAME_HEADER_SIZE || !valid(&decoded))
        return false;
    *frame = decoded;
    return true;
}

bool esp_openclaw_setup_frame_encode(const esp_openclaw_setup_frame_t *frame,
                                     uint8_t *output, size_t capacity, size_t *size)
{
    if (frame == NULL || output == NULL || size == NULL || !valid(frame)) return false;
    size_t encoded_size = ESP_OPENCLAW_SETUP_FRAME_HEADER_SIZE + frame->payload_size;
    if (encoded_size > capacity) return false;
    output[0] = 'O';
    output[1] = 'C';
    output[2] = 1;
    output[3] = (uint8_t)frame->kind;
    output[4] = (uint8_t)(frame->transfer_id >> 24);
    output[5] = (uint8_t)(frame->transfer_id >> 16);
    output[6] = (uint8_t)(frame->transfer_id >> 8);
    output[7] = (uint8_t)frame->transfer_id;
    write16(output + 8, frame->total_size);
    write16(output + 10, frame->offset);
    write16(output + 12, frame->payload_size);
    output[14] = 0;
    output[15] = 0;
    if (frame->payload_size != 0)
        memcpy(output + ESP_OPENCLAW_SETUP_FRAME_HEADER_SIZE, frame->payload, frame->payload_size);
    *size = encoded_size;
    return true;
}
