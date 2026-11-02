#include "esp_openclaw_setup_response.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint64_t generation = UINT64_C(0x0102030405060708);
static const uint8_t challenge[32] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
};

static void rejected(const esp_openclaw_setup_response_t *response, size_t capacity)
{
    uint8_t output[ESP_OPENCLAW_SETUP_RESPONSE_SIZE_MAX], before[sizeof(output)];
    memset(output, 0xa5, sizeof(output)); memcpy(before, output, sizeof(output));
    size_t size = 12345;
    assert(!esp_openclaw_setup_response_encode(response, output, capacity, &size));
    assert(size == 12345 && memcmp(output, before, sizeof(output)) == 0);
}

static void vector(const esp_openclaw_setup_response_t *response, const uint8_t *expected, size_t length)
{
    uint8_t output[ESP_OPENCLAW_SETUP_RESPONSE_SIZE_MAX + 1];
    memset(output, 0xa5, sizeof(output));
    size_t size = 0;
    // An unaligned destination has no special contract. Exact capacity must fit.
    assert(esp_openclaw_setup_response_encode(response, output + 1, length, &size));
    assert(size == length && memcmp(output + 1, expected, length) == 0 && output[0] == 0xa5);
    if (length < ESP_OPENCLAW_SETUP_RESPONSE_SIZE_MAX) assert(output[length + 1] == 0xa5);
    for (size_t capacity = 0; capacity < length; ++capacity) rejected(response, capacity);
}

static void hello_vector(void)
{
    const uint8_t expected[] = {
        1, 1, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 0,
        1, 5, 'W', 'a', 't', 'c', 'h', 2, 'S', '3',
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
        16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
    };
    esp_openclaw_setup_response_t response = {.operation = 1, .generation = generation,
        .body.hello = {.phase = ESP_OPENCLAW_SETUP_RESPONSE_COMPARISON,
            .label = {(const uint8_t *)"Watch", 5}, .model = {(const uint8_t *)"S3", 2},
            .challenge = challenge}};
    vector(&response, expected, sizeof(expected));
    uint8_t label[65]; memset(label, 'x', sizeof(label));
    response.body.hello.label = response.body.hello.model = (esp_openclaw_setup_bytes_t){label, 64};
    uint8_t output[179]; size_t size;
    assert(esp_openclaw_setup_response_encode(&response, output, sizeof(output), &size) && size == 179);
    response.body.hello.label.size = 65; rejected(&response, 596);
    response.body.hello.label.size = 0; rejected(&response, 596);
    response.body.hello.label.size = 64; label[63] = 0; rejected(&response, 596);
    label[63] = 'x'; response.body.hello.model.size = 65; rejected(&response, 596);
    response.body.hello.model.size = 0; rejected(&response, 596);
    response.body.hello.model.size = 64; response.body.hello.model.data = NULL; rejected(&response, 596);
    response.body.hello.model.data = label; response.body.hello.challenge = NULL; rejected(&response, 596);
    response.body.hello.challenge = challenge; response.body.hello.phase = 0; rejected(&response, 596);
    response.body.hello.phase = 1; response.operation_id = 1; rejected(&response, 596);
}

static esp_openclaw_setup_response_t receipt(void)
{
    return (esp_openclaw_setup_response_t){.operation = 4, .code = ESP_OPENCLAW_SETUP_RESPONSE_ACCEPTED,
        .generation = generation, .operation_id = UINT32_C(0x89abcdef),
        .body.status = {.phase = ESP_OPENCLAW_SETUP_RESPONSE_WORKING, .receipt_count = 1,
            .operation = 4, .outcome = ESP_OPENCLAW_SETUP_RESPONSE_PENDING,
            .operation_id = UINT32_C(0x89abcdef), .effects_known = 1,
            .progress = ESP_OPENCLAW_SETUP_RESPONSE_PREPARING}};
}

static void status_vectors(void)
{
    const uint8_t accepted[] = {
        1, 4, 0, 1, 1, 2, 3, 4, 5, 6, 7, 8, 0x89, 0xab, 0xcd, 0xef,
        3, 1, 4, 1, 0x89, 0xab, 0xcd, 0xef, 0, 0, 1, 0, 1, 0, 0, 0,
    };
    esp_openclaw_setup_response_t response = receipt();
    vector(&response, accepted, sizeof(accepted));
    const uint8_t older[] = {
        1, 2, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 0x89, 0xab, 0xcd, 0xef,
        3, 32, 4, 3, 0x89, 0xab, 0xcd, 0xef, 0, 4, 0, 0, 0, 0, 0, 0,
    };
    response.operation = 2; response.code = ESP_OPENCLAW_SETUP_RESPONSE_OK;
    response.body.status.receipt_count = 32; response.body.status.outcome = ESP_OPENCLAW_SETUP_RESPONSE_FAILED;
    response.body.status.detail = ESP_OPENCLAW_SETUP_RESPONSE_DETAIL_TIMEOUT;
    response.body.status.effects_known = 0; response.body.status.progress = 0;
    vector(&response, older, sizeof(older));
    const uint8_t duplicate[] = {
        1, 5, 0, 2, 1, 2, 3, 4, 5, 6, 7, 8, 0xff, 0xff, 0xff, 0xff,
        2, 1, 5, 2, 0xff, 0xff, 0xff, 0xff, 0, 0, 1, 1, 5, 0, 0, 0,
    };
    response = receipt(); response.operation = response.body.status.operation = 5;
    response.code = ESP_OPENCLAW_SETUP_RESPONSE_DUPLICATE;
    response.operation_id = response.body.status.operation_id = UINT32_MAX;
    response.body.status.phase = ESP_OPENCLAW_SETUP_RESPONSE_READY;
    response.body.status.outcome = ESP_OPENCLAW_SETUP_RESPONSE_SUCCEEDED;
    response.body.status.effects_value = 1; response.body.status.progress = ESP_OPENCLAW_SETUP_RESPONSE_TERMINAL;
    vector(&response, duplicate, sizeof(duplicate));
    const uint8_t empty[] = {
        1, 2, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 0,
        2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    response = (esp_openclaw_setup_response_t){.operation = 2, .generation = generation,
        .body.status.phase = ESP_OPENCLAW_SETUP_RESPONSE_READY};
    vector(&response, empty, sizeof(empty));
    response.body.status.effects_value = 1; rejected(&response, 596);
    response = receipt(); response.body.status.effects_known = 2; rejected(&response, 596);
    response = receipt(); response.body.status.receipt_count = 33; rejected(&response, 596);
    response = receipt(); response.body.status.operation_id = 1; rejected(&response, 596);
    response = receipt(); response.body.status.operation = 5; rejected(&response, 596);
    response = receipt(); response.body.status.outcome = 5; rejected(&response, 596);
    response = receipt(); response.body.status.detail = 8; rejected(&response, 596);
    response = receipt(); response.body.status.outcome = ESP_OPENCLAW_SETUP_RESPONSE_FAILED; rejected(&response, 596);
    response = receipt(); response.body.status.detail = ESP_OPENCLAW_SETUP_RESPONSE_DETAIL_FAILED; rejected(&response, 596);
    response = receipt(); response.body.status.progress = 6; rejected(&response, 596);
    response = receipt(); response.body.status.phase = 1; rejected(&response, 596);
}

static void scan_vector(void)
{
    const uint8_t expected[] = {
        1, 3, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 0,
        2, 1, 0, 3, 2, 'A', 0xff, 0xc9, 6, 1, 0, 0x80, 0, 2,
    };
    esp_openclaw_setup_response_t response = {.operation = 3, .generation = generation,
        .body.scan = {.count = 2, .total = 3, .networks = {
            {.ssid_size = 2, .ssid = {'A', 0xff}, .rssi = -55, .channel = 6, .security = 1},
            {.rssi = -128, .security = 2}}}};
    vector(&response, expected, sizeof(expected));
    response.body.scan.count = 16; response.body.scan.total = UINT16_MAX;
    for (unsigned i = 0; i < 16; ++i) {
        response.body.scan.networks[i].ssid_size = 32;
        memset(response.body.scan.networks[i].ssid, 0xff, 32);
    }
    uint8_t output[ESP_OPENCLAW_SETUP_RESPONSE_SIZE_MAX]; size_t size;
    assert(esp_openclaw_setup_response_encode(&response, output, sizeof(output), &size) && size == sizeof(output));
    rejected(&response, sizeof(output) - 1);
    response.body.scan.count = 17; rejected(&response, sizeof(output));
    response.body.scan.count = 16; response.body.scan.total = 15; rejected(&response, sizeof(output));
    response.body.scan.total = 16; response.body.scan.networks[15].ssid_size = 33; rejected(&response, sizeof(output));
    response.body.scan.networks[15].ssid_size = 32; response.body.scan.networks[15].security = 3;
    rejected(&response, sizeof(output));
}

static void errors_and_invalid_headers(void)
{
    esp_openclaw_setup_response_t response = {.code = ESP_OPENCLAW_SETUP_RESPONSE_INVALID};
    uint8_t expected[] = {1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    for (unsigned code = 0; code <= 11; ++code) {
        response.code = (esp_openclaw_setup_response_code_t)(0x100 + code);
        expected[3] = code;
        vector(&response, expected, sizeof(expected));
    }
    response.code = 0x10c; rejected(&response, 596);
    response.code = ESP_OPENCLAW_SETUP_RESPONSE_INVALID; response.generation = 1; rejected(&response, 596);
    response.generation = 0; response.operation_id = 1; rejected(&response, 596);
    response = receipt(); response.generation = 0; rejected(&response, 596);
    response = receipt(); response.operation_id = 0; rejected(&response, 596);
    response = receipt(); response.code = ESP_OPENCLAW_SETUP_RESPONSE_OK; rejected(&response, 596);
    response = receipt(); response.operation = 6; response.code = ESP_OPENCLAW_SETUP_RESPONSE_OK; rejected(&response, 596);
    response = receipt(); response.operation = 7; rejected(&response, 596);
    rejected(NULL, 596);
    response = receipt();
    uint8_t output[596]; size_t size;
    assert(!esp_openclaw_setup_response_encode(&response, NULL, sizeof(output), &size));
    assert(!esp_openclaw_setup_response_encode(&response, output, sizeof(output), NULL));
}

int main(void)
{
    hello_vector(); status_vectors(); scan_vector(); errors_and_invalid_headers();
    puts("setup response: independent vectors, exact bounds and unchanged failure outputs passed");
    return 0;
}
