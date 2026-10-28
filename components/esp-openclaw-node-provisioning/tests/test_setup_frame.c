#include "esp_openclaw_setup_frame.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_wire_vectors(void)
{
    /* Independent wire examples: big-endian fields, fixed reserved bytes. */
    static const uint8_t request[] = {
        'O', 'C', 1, 1, 1, 2, 3, 4, 0, 3, 0, 0, 0, 3, 0, 0, 'a', 'b', 'c',
    };
    static const uint8_t pull[] = {
        'O', 'C', 1, 2, 1, 2, 3, 4, 0, 0, 1, 192, 0, 0, 0, 0,
    };
    static const uint8_t ack[] = {
        'O', 'C', 1, 0x81, 1, 2, 3, 4, 0, 3, 0, 3, 0, 0, 0, 0,
    };
    static const uint8_t response[] = {
        'O', 'C', 1, 0x82, 1, 2, 3, 4, 1, 194, 1, 192, 0, 2, 0, 0, 'o', 'k',
    };
    static const uint8_t error[] = {
        'O', 'C', 1, 0xff, 1, 2, 3, 4, 0, 0, 0, 0, 0, 2, 0, 0, 0, 5,
    };
    struct {
        const uint8_t *wire;
        size_t size;
        esp_openclaw_setup_frame_kind_t kind;
        uint16_t total, offset, payload_size;
    } cases[] = {
        {request, sizeof(request), ESP_OPENCLAW_SETUP_FRAME_REQUEST, 3, 0, 3},
        {pull, sizeof(pull), ESP_OPENCLAW_SETUP_FRAME_PULL, 0, 448, 0},
        {ack, sizeof(ack), ESP_OPENCLAW_SETUP_FRAME_ACK, 3, 3, 0},
        {response, sizeof(response), ESP_OPENCLAW_SETUP_FRAME_RESPONSE, 450, 448, 2},
        {error, sizeof(error), ESP_OPENCLAW_SETUP_FRAME_ERROR, 0, 0, 2},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        esp_openclaw_setup_frame_t frame;
        assert(esp_openclaw_setup_frame_decode(cases[i].wire, cases[i].size, &frame));
        assert(frame.kind == cases[i].kind && frame.transfer_id == UINT32_C(0x01020304));
        assert(frame.total_size == cases[i].total && frame.offset == cases[i].offset);
        assert(frame.payload_size == cases[i].payload_size);
        assert(frame.payload == cases[i].wire + 16);
        uint8_t output[32];
        size_t size = 0;
        assert(esp_openclaw_setup_frame_encode(&frame, output, sizeof(output), &size));
        assert(size == cases[i].size && memcmp(output, cases[i].wire, size) == 0);
    }
}

static void test_size_boundaries(void)
{
    uint8_t payload[449];
    memset(payload, 'x', sizeof(payload));
    uint8_t wire[465];
    esp_openclaw_setup_frame_t frame = {
        .kind = ESP_OPENCLAW_SETUP_FRAME_REQUEST, .transfer_id = UINT32_MAX,
        .total_size = 8192, .offset = 7744, .payload_size = 448, .payload = payload,
    };
    size_t size = 99;
    assert(esp_openclaw_setup_frame_encode(&frame, wire, sizeof(wire), &size));
    assert(size == 464 && size + 16 <= 481); /* Security 2 tag + Bluedroid limit. */
    esp_openclaw_setup_frame_t decoded;
    assert(esp_openclaw_setup_frame_decode(wire, size, &decoded));
    assert(decoded.transfer_id == UINT32_MAX && decoded.offset == 7744);
    assert(memcmp(decoded.payload, payload, 448) == 0);
    assert(!esp_openclaw_setup_frame_decode(wire, size + 1, &decoded));
    assert(!esp_openclaw_setup_frame_encode(&frame, wire, size - 1, &size));
    frame.total_size = 8193;
    assert(!esp_openclaw_setup_frame_encode(&frame, wire, sizeof(wire), &size));
    frame.total_size = 8192;
    frame.offset = 7745;
    assert(!esp_openclaw_setup_frame_encode(&frame, wire, sizeof(wire), &size));
    frame.offset = 0;
    frame.payload_size = 449;
    assert(!esp_openclaw_setup_frame_encode(&frame, wire, sizeof(wire), &size));
    frame.payload_size = 1;
    frame.total_size = 1;
    assert(esp_openclaw_setup_frame_encode(&frame, wire, sizeof(wire), &size));
    assert(esp_openclaw_setup_frame_decode(wire, size, &decoded));
}

static void test_rejections_preserve_outputs(void)
{
    const uint8_t valid[] = {
        'O', 'C', 1, 1, 0, 0, 0, 1, 0, 3, 0, 0, 0, 3, 0, 0, 'a', 'b', 'c',
    };
    struct { size_t offset; uint8_t byte; } corruptions[] = {
        {0, 'X'}, {1, 'X'}, {2, 0}, {2, 2}, {3, 0}, {3, 3},
        {7, 0}, {8, 33}, {9, 0}, {11, 1}, {12, 1}, {13, 2}, {14, 1}, {15, 1},
    };
    esp_openclaw_setup_frame_t sentinel = {.transfer_id = 41};
    for (size_t i = 0; i < sizeof(corruptions) / sizeof(corruptions[0]); ++i) {
        uint8_t wire[sizeof(valid)];
        memcpy(wire, valid, sizeof(wire));
        wire[corruptions[i].offset] = corruptions[i].byte;
        esp_openclaw_setup_frame_t frame;
        memcpy(&frame, &sentinel, sizeof(frame));
        assert(!esp_openclaw_setup_frame_decode(wire, sizeof(wire), &frame));
        assert(memcmp(&frame, &sentinel, sizeof(frame)) == 0);
    }
    for (size_t size = 0; size < sizeof(valid); ++size) {
        esp_openclaw_setup_frame_t frame;
        memcpy(&frame, &sentinel, sizeof(frame));
        assert(!esp_openclaw_setup_frame_decode(valid, size, &frame));
        assert(memcmp(&frame, &sentinel, sizeof(frame)) == 0);
    }
    esp_openclaw_setup_frame_t frame;
    assert(!esp_openclaw_setup_frame_decode(NULL, sizeof(valid), &frame));
    assert(!esp_openclaw_setup_frame_decode(valid, sizeof(valid), NULL));
    assert(esp_openclaw_setup_frame_decode(valid, sizeof(valid), &frame));
    uint8_t wire[sizeof(valid)];
    memset(wire, 0xa5, sizeof(wire));
    size_t size = 42;
    assert(!esp_openclaw_setup_frame_encode(&frame, wire, sizeof(wire) - 1, &size));
    assert(size == 42);
    for (size_t i = 0; i < sizeof(wire); ++i) assert(wire[i] == 0xa5);
    frame.payload = NULL;
    assert(!esp_openclaw_setup_frame_encode(&frame, wire, sizeof(wire), &size));
    assert(!esp_openclaw_setup_frame_encode(NULL, wire, sizeof(wire), &size));
    assert(!esp_openclaw_setup_frame_encode(&frame, NULL, sizeof(wire), &size));
    assert(!esp_openclaw_setup_frame_encode(&frame, wire, sizeof(wire), NULL));
}

static void test_control_frame_shapes(void)
{
    uint8_t wire[32], error[] = {0, 1};
    size_t size;
    esp_openclaw_setup_frame_t frame = {
        .kind = ESP_OPENCLAW_SETUP_FRAME_ERROR, .transfer_id = 1,
        .payload = error, .payload_size = 2,
    };
    for (uint8_t code = 0; code <= 9; ++code) {
        error[1] = code;
        assert(esp_openclaw_setup_frame_encode(&frame, wire, sizeof(wire), &size) ==
            (code >= 1 && code <= 8));
    }
    error[1] = 1;
    frame.total_size = 1;
    assert(!esp_openclaw_setup_frame_encode(&frame, wire, sizeof(wire), &size));
    frame.total_size = 0;
    frame.offset = 1;
    assert(!esp_openclaw_setup_frame_encode(&frame, wire, sizeof(wire), &size));
    frame = (esp_openclaw_setup_frame_t){.kind = ESP_OPENCLAW_SETUP_FRAME_ACK, .transfer_id = 1,
        .total_size = 3, .offset = 4};
    assert(!esp_openclaw_setup_frame_encode(&frame, wire, sizeof(wire), &size));
    frame.offset = 0;
    frame.payload = error;
    frame.payload_size = 1;
    assert(!esp_openclaw_setup_frame_encode(&frame, wire, sizeof(wire), &size));
    frame = (esp_openclaw_setup_frame_t){.kind = ESP_OPENCLAW_SETUP_FRAME_PULL, .transfer_id = 1,
        .offset = 8192};
    assert(!esp_openclaw_setup_frame_encode(&frame, wire, sizeof(wire), &size));
    frame.offset = 8191;
    assert(esp_openclaw_setup_frame_encode(&frame, wire, sizeof(wire), &size));
    frame.total_size = 1;
    assert(!esp_openclaw_setup_frame_encode(&frame, wire, sizeof(wire), &size));
}

int main(void)
{
    test_wire_vectors();
    test_size_boundaries();
    test_rejections_preserve_outputs();
    test_control_frame_shapes();
    puts("setup frame tests passed");
    return 0;
}
