#include "esp_openclaw_setup_request.h"

#include <stdbool.h>
#include <string.h>

#include "esp_openclaw_setup_frame.h"

static uint64_t read_integer(const uint8_t *input, size_t size)
{
    uint64_t value = 0;
    for (size_t i = 0; i < size; ++i) value = (value << 8) | input[i];
    return value;
}

static bool read_field(const uint8_t **cursor, size_t *remaining, size_t minimum,
                       size_t maximum, esp_openclaw_setup_bytes_t *field)
{
    if (*remaining < 2) return false;
    size_t size = (size_t)read_integer(*cursor, 2);
    if (size < minimum || size > maximum || size > *remaining - 2) return false;
    const uint8_t *data = *cursor + 2;
    /* The native Wi-Fi and setup-code owners accept C strings. */
    if (memchr(data, 0, size) != NULL) return false;
    *field = (esp_openclaw_setup_bytes_t){.data = data, .size = size};
    *cursor = data + size;
    *remaining -= 2 + size;
    return true;
}

esp_openclaw_setup_request_result_t esp_openclaw_setup_request_decode(
    const uint8_t *input, size_t size, esp_openclaw_setup_request_t *request)
{
    if (input == NULL || request == NULL || size < ESP_OPENCLAW_SETUP_REQUEST_HEADER_SIZE)
        return ESP_OPENCLAW_SETUP_REQUEST_INVALID;
    if (size > ESP_OPENCLAW_SETUP_MESSAGE_SIZE_MAX)
        return ESP_OPENCLAW_SETUP_REQUEST_TOO_LARGE;
    if (input[0] != 1) return ESP_OPENCLAW_SETUP_REQUEST_UNSUPPORTED_VERSION;
    if (input[2] != 0 || input[3] != 0) return ESP_OPENCLAW_SETUP_REQUEST_INVALID;

    esp_openclaw_setup_request_t decoded = {
        .kind = (esp_openclaw_setup_request_kind_t)input[1],
        .generation = read_integer(input + 4, 8),
        .operation_id = (uint32_t)read_integer(input + 12, 4),
    };
    const uint8_t *cursor = input + ESP_OPENCLAW_SETUP_REQUEST_HEADER_SIZE;
    size_t remaining = size - ESP_OPENCLAW_SETUP_REQUEST_HEADER_SIZE;
    switch (decoded.kind) {
    case ESP_OPENCLAW_SETUP_REQUEST_HELLO:
        if (decoded.generation != 0 || decoded.operation_id != 0)
            return ESP_OPENCLAW_SETUP_REQUEST_INVALID;
        break;
    case ESP_OPENCLAW_SETUP_REQUEST_STATUS:
        if (decoded.generation == 0) return ESP_OPENCLAW_SETUP_REQUEST_INVALID;
        break;
    case ESP_OPENCLAW_SETUP_REQUEST_WIFI_SCAN:
    case ESP_OPENCLAW_SETUP_REQUEST_CANCEL:
        if (decoded.generation == 0 || decoded.operation_id != 0)
            return ESP_OPENCLAW_SETUP_REQUEST_INVALID;
        break;
    case ESP_OPENCLAW_SETUP_REQUEST_WIFI_APPLY:
        if (decoded.generation == 0 || decoded.operation_id == 0 ||
            !read_field(&cursor, &remaining, 1, ESP_OPENCLAW_SETUP_SSID_SIZE_MAX,
                        &decoded.payload.wifi.ssid) ||
            !read_field(&cursor, &remaining, 0, ESP_OPENCLAW_SETUP_PASSWORD_SIZE_MAX,
                        &decoded.payload.wifi.password)) return ESP_OPENCLAW_SETUP_REQUEST_INVALID;
        break;
    case ESP_OPENCLAW_SETUP_REQUEST_GATEWAY_APPLY:
        if (decoded.generation == 0 || decoded.operation_id == 0 ||
            !read_field(&cursor, &remaining, 1, ESP_OPENCLAW_SETUP_CODE_SIZE_MAX,
                        &decoded.payload.setup_code)) return ESP_OPENCLAW_SETUP_REQUEST_INVALID;
        break;
    default:
        return ESP_OPENCLAW_SETUP_REQUEST_UNKNOWN_OPERATION;
    }
    if (remaining != 0) return ESP_OPENCLAW_SETUP_REQUEST_INVALID;
    *request = decoded;
    return ESP_OPENCLAW_SETUP_REQUEST_OK;
}
