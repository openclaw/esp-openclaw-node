#include "esp_openclaw_setup_request.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Independent application wire fixtures, without a matching encoder. */
static const uint8_t hello[] = {1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t status[] = {1, 2, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 0};
static const uint8_t scan[] = {1, 3, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 0};
static const uint8_t wifi[] = {
    1, 4, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 0x89, 0xab, 0xcd, 0xef,
    0, 4, ' ', 'A', 0xff, ' ', 0, 3, ' ', 'p', ' ',
};
static const uint8_t gateway[] = {
    1, 5, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 0x89, 0xab, 0xcd, 0xef,
    0, 4, 'a', 'b', '_', '-',
};
static const uint8_t cancel[] = {1, 6, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 0};

static void rejected(const uint8_t *wire, size_t size, esp_openclaw_setup_request_result_t result)
{
    esp_openclaw_setup_request_t request, before;
    memset(&request, 0xa5, sizeof(request));
    memcpy(&before, &request, sizeof(before));
    assert(esp_openclaw_setup_request_decode(wire, size, &request) == result);
    assert(memcmp(&request, &before, sizeof(request)) == 0);
}

static void test_wire_vectors(void)
{
    struct {
        const uint8_t *wire;
        size_t size;
        esp_openclaw_setup_request_kind_t kind;
    } cases[] = {
        {hello, sizeof(hello), ESP_OPENCLAW_SETUP_REQUEST_HELLO},
        {status, sizeof(status), ESP_OPENCLAW_SETUP_REQUEST_STATUS},
        {scan, sizeof(scan), ESP_OPENCLAW_SETUP_REQUEST_WIFI_SCAN},
        {wifi, sizeof(wifi), ESP_OPENCLAW_SETUP_REQUEST_WIFI_APPLY},
        {gateway, sizeof(gateway), ESP_OPENCLAW_SETUP_REQUEST_GATEWAY_APPLY},
        {cancel, sizeof(cancel), ESP_OPENCLAW_SETUP_REQUEST_CANCEL},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        esp_openclaw_setup_request_t request;
        assert(esp_openclaw_setup_request_decode(cases[i].wire, cases[i].size, &request) ==
               ESP_OPENCLAW_SETUP_REQUEST_OK);
        assert(request.kind == cases[i].kind);
        assert(request.generation == (i == 0 ? 0 : UINT64_C(0x0102030405060708)));
        assert(request.operation_id == (i == 3 || i == 4 ? UINT32_C(0x89abcdef) : 0));
        for (size_t size = 0; size < cases[i].size; ++size)
            rejected(cases[i].wire, size, ESP_OPENCLAW_SETUP_REQUEST_INVALID);
        uint8_t extended[sizeof(wifi) + 1];
        memcpy(extended, cases[i].wire, cases[i].size);
        extended[cases[i].size] = 0;
        rejected(extended, cases[i].size + 1, ESP_OPENCLAW_SETUP_REQUEST_INVALID);
    }

    esp_openclaw_setup_request_t request;
    assert(esp_openclaw_setup_request_decode(wifi, sizeof(wifi), &request) ==
           ESP_OPENCLAW_SETUP_REQUEST_OK);
    assert(request.payload.wifi.ssid.data == wifi + 18 && request.payload.wifi.ssid.size == 4);
    assert(memcmp(request.payload.wifi.ssid.data, " A\xff ", 4) == 0);
    assert(request.payload.wifi.password.data == wifi + 24 && request.payload.wifi.password.size == 3);
    assert(memcmp(request.payload.wifi.password.data, " p ", 3) == 0);
    assert(esp_openclaw_setup_request_decode(gateway, sizeof(gateway), &request) ==
           ESP_OPENCLAW_SETUP_REQUEST_OK);
    assert(request.payload.setup_code.data == gateway + 18 && request.payload.setup_code.size == 4);
    assert(memcmp(request.payload.setup_code.data, "ab_-", 4) == 0);
}

static void test_header_rejections(void)
{
    struct { size_t offset; uint8_t value; esp_openclaw_setup_request_result_t result; } cases[] = {
        {0, 0, ESP_OPENCLAW_SETUP_REQUEST_UNSUPPORTED_VERSION},
        {0, 2, ESP_OPENCLAW_SETUP_REQUEST_UNSUPPORTED_VERSION},
        {1, 0, ESP_OPENCLAW_SETUP_REQUEST_UNKNOWN_OPERATION},
        {1, 7, ESP_OPENCLAW_SETUP_REQUEST_UNKNOWN_OPERATION},
        {1, 0xff, ESP_OPENCLAW_SETUP_REQUEST_UNKNOWN_OPERATION},
        {2, 1, ESP_OPENCLAW_SETUP_REQUEST_INVALID},
        {3, 1, ESP_OPENCLAW_SETUP_REQUEST_INVALID},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint8_t wire[sizeof(status)];
        memcpy(wire, status, sizeof(wire));
        wire[cases[i].offset] = cases[i].value;
        rejected(wire, sizeof(wire), cases[i].result);
    }
    uint8_t oversized[8193] = {0};
    rejected(oversized, sizeof(oversized), ESP_OPENCLAW_SETUP_REQUEST_TOO_LARGE);
    rejected(NULL, sizeof(status), ESP_OPENCLAW_SETUP_REQUEST_INVALID);
    assert(esp_openclaw_setup_request_decode(status, sizeof(status), NULL) ==
           ESP_OPENCLAW_SETUP_REQUEST_INVALID);
}

static void test_identity_rules(void)
{
    uint8_t wire[sizeof(wifi)];
    const uint8_t *commands[] = {status, scan, wifi, gateway, cancel};
    const size_t sizes[] = {sizeof(status), sizeof(scan), sizeof(wifi), sizeof(gateway), sizeof(cancel)};
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        memcpy(wire, commands[i], sizes[i]);
        memset(wire + 4, 0, 8);
        rejected(wire, sizes[i], ESP_OPENCLAW_SETUP_REQUEST_INVALID);
    }
    for (size_t offset = 4; offset < 16; ++offset) {
        memcpy(wire, hello, sizeof(hello));
        wire[offset] = 1;
        rejected(wire, sizeof(hello), ESP_OPENCLAW_SETUP_REQUEST_INVALID);
    }
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        memcpy(wire, commands[i], sizes[i]);
        memset(wire + 12, i == 1 || i == 4 ? 0xff : 0, 4);
        if (i != 0) rejected(wire, sizes[i], ESP_OPENCLAW_SETUP_REQUEST_INVALID);
    }
    esp_openclaw_setup_request_t request;
    memcpy(wire, status, sizeof(status));
    memset(wire + 4, 0xff, 12);
    assert(esp_openclaw_setup_request_decode(wire, sizeof(status), &request) ==
           ESP_OPENCLAW_SETUP_REQUEST_OK);
    assert(request.generation == UINT64_MAX && request.operation_id == UINT32_MAX);

    /* No alignment contract; identities above JavaScript's exact-number range survive. */
    uint8_t unaligned[sizeof(gateway) + 1];
    memcpy(unaligned + 1, gateway, sizeof(gateway));
    assert(esp_openclaw_setup_request_decode(unaligned + 1, sizeof(gateway), &request) ==
           ESP_OPENCLAW_SETUP_REQUEST_OK);
    assert(request.generation == UINT64_C(0x0102030405060708));
}

static void test_wifi_fields(void)
{
    uint8_t wire[16 + 2 + 33 + 2 + 65];
    memcpy(wire, wifi, 16);
    wire[16] = 0;
    wire[17] = 32;
    memset(wire + 18, 's', 32);
    wire[50] = 0;
    wire[51] = 64;
    memset(wire + 52, 'f', 64);
    esp_openclaw_setup_request_t request;
    assert(esp_openclaw_setup_request_decode(wire, 116, &request) == ESP_OPENCLAW_SETUP_REQUEST_OK);
    assert(request.payload.wifi.ssid.size == 32 && request.payload.wifi.password.size == 64);
    for (size_t size = 0; size < 116; ++size)
        rejected(wire, size, ESP_OPENCLAW_SETUP_REQUEST_INVALID);
    for (size_t offset = 18; offset < 116; ++offset) {
        if (offset == 50 || offset == 51) continue;
        uint8_t saved = wire[offset];
        wire[offset] = 0;
        rejected(wire, 116, ESP_OPENCLAW_SETUP_REQUEST_INVALID);
        wire[offset] = saved;
    }
    wire[51] = 0;
    assert(esp_openclaw_setup_request_decode(wire, 52, &request) == ESP_OPENCLAW_SETUP_REQUEST_OK);
    assert(request.payload.wifi.password.size == 0);
    wire[51] = 65;
    rejected(wire, 117, ESP_OPENCLAW_SETUP_REQUEST_INVALID);
    wire[51] = 64;
    wire[17] = 33;
    rejected(wire, 116, ESP_OPENCLAW_SETUP_REQUEST_INVALID);
    wire[17] = 0;
    rejected(wire, 116, ESP_OPENCLAW_SETUP_REQUEST_INVALID);
    wire[16] = 0xff;
    wire[17] = 0xff;
    rejected(wire, 116, ESP_OPENCLAW_SETUP_REQUEST_INVALID);
    wire[16] = 0;
    wire[17] = 32;
    wire[50] = 0xff;
    wire[51] = 0xff;
    rejected(wire, 116, ESP_OPENCLAW_SETUP_REQUEST_INVALID);
}

static void test_setup_code_bounds(void)
{
    uint8_t wire[16 + 2 + 4097];
    memcpy(wire, gateway, 16);
    wire[16] = 0x10;
    wire[17] = 0;
    memset(wire + 18, 'x', 4097);
    esp_openclaw_setup_request_t request;
    assert(esp_openclaw_setup_request_decode(wire, 4114, &request) == ESP_OPENCLAW_SETUP_REQUEST_OK);
    assert(request.payload.setup_code.size == 4096 && request.payload.setup_code.data == wire + 18);
    for (size_t size = 0; size < 4114; ++size)
        rejected(wire, size, ESP_OPENCLAW_SETUP_REQUEST_INVALID);
    const size_t zero_offsets[] = {18, 2048, 4113};
    for (size_t i = 0; i < sizeof(zero_offsets) / sizeof(zero_offsets[0]); ++i) {
        wire[zero_offsets[i]] = 0;
        rejected(wire, 4114, ESP_OPENCLAW_SETUP_REQUEST_INVALID);
        wire[zero_offsets[i]] = 'x';
    }
    wire[17] = 1;
    rejected(wire, sizeof(wire), ESP_OPENCLAW_SETUP_REQUEST_INVALID);
    wire[16] = 0xff;
    wire[17] = 0xff;
    rejected(wire, sizeof(wire), ESP_OPENCLAW_SETUP_REQUEST_INVALID);
    wire[16] = 0;
    wire[17] = 0;
    rejected(wire, 18, ESP_OPENCLAW_SETUP_REQUEST_INVALID);
    wire[17] = 1;
    assert(esp_openclaw_setup_request_decode(wire, 19, &request) == ESP_OPENCLAW_SETUP_REQUEST_OK);
    assert(request.payload.setup_code.size == 1);
}

int main(void)
{
    test_wire_vectors();
    test_header_rejections();
    test_identity_rules();
    test_wifi_fields();
    test_setup_code_bounds();
    puts("setup request tests passed");
    return 0;
}
