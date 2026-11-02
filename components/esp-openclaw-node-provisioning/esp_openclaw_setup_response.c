#include "esp_openclaw_setup_response.h"

#include <string.h>

static bool error_code(esp_openclaw_setup_response_code_t code)
{
    switch (code) {
    case ESP_OPENCLAW_SETUP_RESPONSE_INVALID:
    case ESP_OPENCLAW_SETUP_RESPONSE_VERSION:
    case ESP_OPENCLAW_SETUP_RESPONSE_OPERATION:
    case ESP_OPENCLAW_SETUP_RESPONSE_TOO_LARGE:
    case ESP_OPENCLAW_SETUP_RESPONSE_CONFIRM:
    case ESP_OPENCLAW_SETUP_RESPONSE_BUSY:
    case ESP_OPENCLAW_SETUP_RESPONSE_FULL:
    case ESP_OPENCLAW_SETUP_RESPONSE_NOT_FOUND:
    case ESP_OPENCLAW_SETUP_RESPONSE_UNSUPPORTED:
    case ESP_OPENCLAW_SETUP_RESPONSE_UNAVAILABLE:
    case ESP_OPENCLAW_SETUP_RESPONSE_INTERNAL:
    case ESP_OPENCLAW_SETUP_RESPONSE_STALE: return true;
    default: return false;
    }
}

static bool phase_valid(esp_openclaw_setup_response_phase_t phase)
{
    return phase == ESP_OPENCLAW_SETUP_RESPONSE_COMPARISON ||
        phase == ESP_OPENCLAW_SETUP_RESPONSE_READY || phase == ESP_OPENCLAW_SETUP_RESPONSE_WORKING;
}

static bool text_valid(esp_openclaw_setup_bytes_t text)
{
    return text.data != NULL && text.size > 0 && text.size <= ESP_OPENCLAW_SETUP_RESPONSE_TEXT_MAX &&
        memchr(text.data, 0, text.size) == NULL;
}

static bool status_valid(const esp_openclaw_setup_response_t *response)
{
    const esp_openclaw_setup_response_status_t *status = &response->body.status;
    if (!phase_valid(status->phase) || status->phase == ESP_OPENCLAW_SETUP_RESPONSE_COMPARISON ||
        status->receipt_count > 32 || status->detail < ESP_OPENCLAW_SETUP_RESPONSE_DETAIL_NONE ||
        status->detail > ESP_OPENCLAW_SETUP_RESPONSE_DETAIL_FAILED ||
        status->progress < ESP_OPENCLAW_SETUP_RESPONSE_PROGRESS_UNKNOWN ||
        status->progress > ESP_OPENCLAW_SETUP_RESPONSE_TERMINAL ||
        (status->effects_known & ~ESP_OPENCLAW_SETUP_RESPONSE_SAVE_CONFIRMED) != 0 ||
        (status->effects_value & ~status->effects_known) != 0) return false;
    if (status->outcome == ESP_OPENCLAW_SETUP_RESPONSE_NO_RECEIPT)
        return response->operation == ESP_OPENCLAW_SETUP_REQUEST_STATUS && response->operation_id == 0 &&
            status->phase == ESP_OPENCLAW_SETUP_RESPONSE_READY && status->receipt_count == 0 &&
            status->operation == 0 && status->operation_id == 0 &&
            status->detail == ESP_OPENCLAW_SETUP_RESPONSE_DETAIL_NONE && status->effects_known == 0 &&
            status->progress == ESP_OPENCLAW_SETUP_RESPONSE_PROGRESS_UNKNOWN;
    if (status->receipt_count == 0 || status->operation_id == 0 ||
        (status->operation != ESP_OPENCLAW_SETUP_REQUEST_WIFI_APPLY &&
         status->operation != ESP_OPENCLAW_SETUP_REQUEST_GATEWAY_APPLY) ||
        status->outcome < ESP_OPENCLAW_SETUP_RESPONSE_PENDING ||
        status->outcome > ESP_OPENCLAW_SETUP_RESPONSE_UNKNOWN ||
        (response->operation_id != 0 && response->operation_id != status->operation_id) ||
        (response->operation != ESP_OPENCLAW_SETUP_REQUEST_STATUS &&
         response->operation != status->operation)) return false;
    return status->outcome == ESP_OPENCLAW_SETUP_RESPONSE_FAILED ?
        status->detail != ESP_OPENCLAW_SETUP_RESPONSE_DETAIL_NONE :
        status->detail == ESP_OPENCLAW_SETUP_RESPONSE_DETAIL_NONE;
}

static bool response_size(const esp_openclaw_setup_response_t *response, size_t *size)
{
    *size = ESP_OPENCLAW_SETUP_RESPONSE_HEADER_SIZE;
    if (response->operation > ESP_OPENCLAW_SETUP_REQUEST_CANCEL) return false;
    if (error_code(response->code)) {
        if (response->operation == 0 || response->operation == ESP_OPENCLAW_SETUP_REQUEST_HELLO)
            return response->generation == 0 && response->operation_id == 0;
        if (response->generation == 0) return false;
        return response->operation == ESP_OPENCLAW_SETUP_REQUEST_STATUS ||
            ((response->operation == ESP_OPENCLAW_SETUP_REQUEST_WIFI_APPLY ||
              response->operation == ESP_OPENCLAW_SETUP_REQUEST_GATEWAY_APPLY) ?
                response->operation_id != 0 : response->operation_id == 0);
    }
    if (response->generation == 0) return false;
    bool mutation = response->operation == ESP_OPENCLAW_SETUP_REQUEST_WIFI_APPLY ||
        response->operation == ESP_OPENCLAW_SETUP_REQUEST_GATEWAY_APPLY;
    if (mutation) {
        if ((response->code != ESP_OPENCLAW_SETUP_RESPONSE_ACCEPTED &&
             response->code != ESP_OPENCLAW_SETUP_RESPONSE_DUPLICATE) || response->operation_id == 0)
            return false;
    } else if (response->code != ESP_OPENCLAW_SETUP_RESPONSE_OK) return false;
    switch (response->operation) {
    case ESP_OPENCLAW_SETUP_REQUEST_HELLO:
        if (response->operation_id != 0 || !phase_valid(response->body.hello.phase) ||
            !text_valid(response->body.hello.label) || !text_valid(response->body.hello.model) ||
            response->body.hello.challenge == NULL) return false;
        *size += 3 + response->body.hello.label.size + response->body.hello.model.size +
            ESP_OPENCLAW_SETUP_RESPONSE_CHALLENGE_SIZE;
        return true;
    case ESP_OPENCLAW_SETUP_REQUEST_STATUS:
    case ESP_OPENCLAW_SETUP_REQUEST_WIFI_APPLY:
    case ESP_OPENCLAW_SETUP_REQUEST_GATEWAY_APPLY:
        if (!status_valid(response)) return false;
        *size += 16;
        return true;
    case ESP_OPENCLAW_SETUP_REQUEST_WIFI_SCAN:
        if (response->operation_id != 0 || response->body.scan.count > ESP_OPENCLAW_SETUP_RESPONSE_SCAN_MAX ||
            response->body.scan.total < response->body.scan.count) return false;
        *size += 4;
        for (unsigned i = 0; i < response->body.scan.count; ++i) {
            const esp_openclaw_setup_response_network_t *network = &response->body.scan.networks[i];
            if (network->ssid_size > ESP_OPENCLAW_SETUP_SSID_SIZE_MAX ||
                network->security < ESP_OPENCLAW_SETUP_RESPONSE_NETWORK_OPEN ||
                network->security > ESP_OPENCLAW_SETUP_RESPONSE_NETWORK_UNSUPPORTED) return false;
            *size += 4 + network->ssid_size;
        }
        return true;
    default: return false;
    }
}

static void integer(uint8_t *output, uint64_t value, size_t size)
{
    for (size_t i = size; i > 0; --i) { output[i - 1] = (uint8_t)value; value >>= 8; }
}

bool esp_openclaw_setup_response_encode(const esp_openclaw_setup_response_t *response,
    uint8_t *output, size_t capacity, size_t *output_size)
{
    size_t size;
    if (response == NULL || output == NULL || output_size == NULL ||
        !response_size(response, &size) || capacity < size) return false;
    output[0] = 1;
    output[1] = response->operation;
    integer(output + 2, response->code, 2);
    integer(output + 4, response->generation, 8);
    integer(output + 12, response->operation_id, 4);
    uint8_t *body = output + ESP_OPENCLAW_SETUP_RESPONSE_HEADER_SIZE;
    if (error_code(response->code)) { *output_size = size; return true; }
    if (response->operation == ESP_OPENCLAW_SETUP_REQUEST_HELLO) {
        *body++ = response->body.hello.phase;
        *body++ = (uint8_t)response->body.hello.label.size;
        memcpy(body, response->body.hello.label.data, response->body.hello.label.size);
        body += response->body.hello.label.size;
        *body++ = (uint8_t)response->body.hello.model.size;
        memcpy(body, response->body.hello.model.data, response->body.hello.model.size);
        body += response->body.hello.model.size;
        memcpy(body, response->body.hello.challenge, ESP_OPENCLAW_SETUP_RESPONSE_CHALLENGE_SIZE);
    } else if (response->operation == ESP_OPENCLAW_SETUP_REQUEST_WIFI_SCAN) {
        *body++ = response->body.scan.count;
        *body++ = response->body.scan.total > response->body.scan.count;
        integer(body, response->body.scan.total, 2); body += 2;
        for (unsigned i = 0; i < response->body.scan.count; ++i) {
            const esp_openclaw_setup_response_network_t *network = &response->body.scan.networks[i];
            *body++ = network->ssid_size;
            memcpy(body, network->ssid, network->ssid_size); body += network->ssid_size;
            *body++ = (uint8_t)network->rssi;
            *body++ = network->channel;
            *body++ = network->security;
        }
    } else {
        const esp_openclaw_setup_response_status_t *status = &response->body.status;
        body[0] = status->phase; body[1] = status->receipt_count;
        body[2] = status->operation; body[3] = status->outcome;
        integer(body + 4, status->operation_id, 4);
        integer(body + 8, status->detail, 2);
        body[10] = status->effects_known; body[11] = status->effects_value;
        body[12] = status->progress;
        memset(body + 13, 0, 3);
    }
    *output_size = size;
    return true;
}
