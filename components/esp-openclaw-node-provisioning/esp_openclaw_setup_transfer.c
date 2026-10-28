#include "esp_openclaw_setup_transfer.h"

#include <string.h>

static void wipe_buffers(esp_openclaw_setup_transfer_t *transfer)
{
    /* Credentials must not survive retirement, including unused buffer tails. */
    volatile uint8_t *request = transfer->request;
    volatile uint8_t *response = transfer->response;
    for (size_t i = 0; i < ESP_OPENCLAW_SETUP_MESSAGE_SIZE_MAX; ++i) {
        request[i] = 0;
        response[i] = 0;
    }
}

bool esp_openclaw_setup_transfer_init(esp_openclaw_setup_transfer_t *transfer,
                                     esp_openclaw_setup_session_t *session)
{
    if (transfer == NULL || session == NULL) return false;
    memset(transfer, 0, sizeof(*transfer));
    transfer->session = session;
    return true;
}

void esp_openclaw_setup_transfer_close(esp_openclaw_setup_transfer_t *transfer)
{
    wipe_buffers(transfer);
    transfer->active = false;
    transfer->phase = ESP_OPENCLAW_TRANSFER_EMPTY;
    transfer->received = transfer->total = transfer->response_size = 0;
    transfer->error = 0;
    /* Retain the generation so close/open cannot reuse its transfer IDs. */
}

bool esp_openclaw_setup_transfer_poll(esp_openclaw_setup_transfer_t *transfer)
{
    if (!transfer->active) return false;
    if (esp_openclaw_setup_authorize_peer(transfer->session, transfer->token.generation,
            transfer->token.peer) == ESP_OPENCLAW_SETUP_OK) return true;
    esp_openclaw_setup_transfer_close(transfer);
    return false;
}

bool esp_openclaw_setup_transfer_open(esp_openclaw_setup_transfer_t *transfer,
                                     uint64_t generation, uint64_t peer)
{
    if (transfer->active || generation <= transfer->token.generation ||
        esp_openclaw_setup_authorize_peer(transfer->session, generation, peer) !=
            ESP_OPENCLAW_SETUP_OK) return false;
    transfer->token = (esp_openclaw_setup_transfer_token_t){.generation = generation, .peer = peer};
    transfer->active = true;
    return true;
}

static esp_openclaw_setup_frame_error_t receive_request(esp_openclaw_setup_transfer_t *transfer,
    const esp_openclaw_setup_frame_t *frame, bool *handoff)
{
    if (frame->transfer_id < transfer->token.transfer_id) return ESP_OPENCLAW_SETUP_FRAME_STALE;
    if (frame->transfer_id > transfer->token.transfer_id) {
        if (frame->offset != 0) return ESP_OPENCLAW_SETUP_FRAME_INVALID;
        if (transfer->phase == ESP_OPENCLAW_TRANSFER_RECEIVING ||
            transfer->phase == ESP_OPENCLAW_TRANSFER_PENDING) return ESP_OPENCLAW_SETUP_FRAME_BUSY;
        wipe_buffers(transfer);
        transfer->token.transfer_id = frame->transfer_id;
        transfer->total = frame->total_size;
        transfer->received = transfer->response_size = 0;
        transfer->error = 0;
        transfer->phase = ESP_OPENCLAW_TRANSFER_RECEIVING;
    }
    if (frame->total_size != transfer->total) return ESP_OPENCLAW_SETUP_FRAME_CONFLICT;
    if (frame->offset > transfer->received) return ESP_OPENCLAW_SETUP_FRAME_INVALID;
    if (frame->offset < transfer->received) {
        if (frame->payload_size > transfer->received - frame->offset)
            return ESP_OPENCLAW_SETUP_FRAME_CONFLICT;
        return memcmp(transfer->request + frame->offset, frame->payload, frame->payload_size) == 0 ?
            0 : ESP_OPENCLAW_SETUP_FRAME_CONFLICT;
    }
    memcpy(transfer->request + transfer->received, frame->payload, frame->payload_size);
    transfer->received += frame->payload_size;
    if (transfer->received == transfer->total) {
        transfer->phase = ESP_OPENCLAW_TRANSFER_PENDING;
        *handoff = true;
    }
    return 0;
}

static esp_openclaw_setup_frame_error_t prepare_pull(esp_openclaw_setup_transfer_t *transfer,
    const esp_openclaw_setup_frame_t *frame, esp_openclaw_setup_frame_t *reply)
{
    if (frame->transfer_id != transfer->token.transfer_id) return ESP_OPENCLAW_SETUP_FRAME_STALE;
    if (transfer->phase == ESP_OPENCLAW_TRANSFER_RECEIVING ||
        transfer->phase == ESP_OPENCLAW_TRANSFER_PENDING) return ESP_OPENCLAW_SETUP_FRAME_PENDING;
    if (transfer->phase == ESP_OPENCLAW_TRANSFER_FAILED) return transfer->error;
    if (frame->offset >= transfer->response_size) return ESP_OPENCLAW_SETUP_FRAME_INVALID;
    uint16_t size = transfer->response_size - frame->offset;
    if (size > ESP_OPENCLAW_SETUP_FRAME_PAYLOAD_MAX) size = ESP_OPENCLAW_SETUP_FRAME_PAYLOAD_MAX;
    *reply = (esp_openclaw_setup_frame_t){
        .kind = ESP_OPENCLAW_SETUP_FRAME_RESPONSE, .transfer_id = frame->transfer_id,
        .total_size = transfer->response_size, .offset = frame->offset,
        .payload_size = size, .payload = transfer->response + frame->offset,
    };
    return 0;
}

bool esp_openclaw_setup_transfer_receive(esp_openclaw_setup_transfer_t *transfer,
    uint64_t generation, uint64_t peer, const uint8_t *input, size_t input_size,
    uint8_t *output, size_t capacity, size_t *output_size,
    esp_openclaw_setup_transfer_request_t *request)
{
    if (output == NULL || output_size == NULL || request == NULL ||
        capacity < ESP_OPENCLAW_SETUP_FRAME_SIZE_MAX || !esp_openclaw_setup_transfer_poll(transfer) ||
        generation != transfer->token.generation || peer != transfer->token.peer) return false;
    esp_openclaw_setup_frame_t frame;
    if (!esp_openclaw_setup_frame_decode(input, input_size, &frame)) return false;

    bool handoff = false;
    esp_openclaw_setup_frame_t reply = {0};
    esp_openclaw_setup_frame_error_t error;
    if (frame.kind == ESP_OPENCLAW_SETUP_FRAME_REQUEST) {
        error = receive_request(transfer, &frame, &handoff);
        reply = (esp_openclaw_setup_frame_t){
            .kind = ESP_OPENCLAW_SETUP_FRAME_ACK, .transfer_id = frame.transfer_id,
            .total_size = transfer->total, .offset = transfer->received,
        };
    } else if (frame.kind == ESP_OPENCLAW_SETUP_FRAME_PULL) {
        error = prepare_pull(transfer, &frame, &reply);
    } else {
        error = ESP_OPENCLAW_SETUP_FRAME_INVALID;
    }
    uint8_t error_bytes[2] = {0, (uint8_t)error};
    if (error != 0) reply = (esp_openclaw_setup_frame_t){
        .kind = ESP_OPENCLAW_SETUP_FRAME_ERROR, .transfer_id = frame.transfer_id,
        .payload_size = sizeof(error_bytes), .payload = error_bytes,
    };

    size_t size;
    if (!esp_openclaw_setup_transfer_poll(transfer) ||
        !esp_openclaw_setup_frame_encode(&reply, output, capacity, &size)) return false;
    *output_size = size;
    *request = (esp_openclaw_setup_transfer_request_t){0};
    if (handoff) *request = (esp_openclaw_setup_transfer_request_t){
        .token = transfer->token, .data = transfer->request, .size = transfer->total,
    };
    return true;
}

bool esp_openclaw_setup_transfer_complete(esp_openclaw_setup_transfer_t *transfer,
    esp_openclaw_setup_transfer_token_t token, const uint8_t *response, size_t size)
{
    if (!esp_openclaw_setup_transfer_poll(transfer) ||
        token.generation != transfer->token.generation || token.peer != transfer->token.peer ||
        token.transfer_id != transfer->token.transfer_id ||
        transfer->phase != ESP_OPENCLAW_TRANSFER_PENDING) return false;
    if (size == 0 || response == NULL || size > ESP_OPENCLAW_SETUP_MESSAGE_SIZE_MAX) {
        transfer->error = size > ESP_OPENCLAW_SETUP_MESSAGE_SIZE_MAX ?
            ESP_OPENCLAW_SETUP_FRAME_TOO_LARGE : ESP_OPENCLAW_SETUP_FRAME_INTERNAL;
        transfer->phase = ESP_OPENCLAW_TRANSFER_FAILED;
        esp_openclaw_setup_transfer_poll(transfer);
        return false;
    }
    memcpy(transfer->response, response, size);
    transfer->response_size = (uint16_t)size;
    transfer->phase = ESP_OPENCLAW_TRANSFER_READY;
    return esp_openclaw_setup_transfer_poll(transfer);
}
