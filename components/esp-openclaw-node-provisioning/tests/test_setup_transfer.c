#include "esp_openclaw_setup_transfer.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    esp_openclaw_setup_session_t session;
    esp_openclaw_setup_transfer_t transfer;
    uint64_t generation, now;
    bool authenticated;
    unsigned revoke_after_checks, canceled;
    uint8_t output[ESP_OPENCLAW_SETUP_FRAME_SIZE_MAX];
    size_t output_size;
    esp_openclaw_setup_transfer_request_t request;
} fixture_t;

static uint64_t now_ms(void *context) { return ((fixture_t *)context)->now; }

static bool authenticated(uint64_t peer, void *context)
{
    fixture_t *f = context;
    bool result = f->authenticated && (peer == 1 || peer == 2);
    if (f->revoke_after_checks != 0 && --f->revoke_after_checks == 0) f->authenticated = false;
    return result;
}

static void cancel(esp_openclaw_setup_token_t token, void *context)
{
    fixture_t *f = context;
    assert(token.generation == f->generation);
    ++f->canceled;
}

static void initialize(fixture_t *f)
{
    memset(f, 0, sizeof(*f));
    f->now = 100;
    f->authenticated = true;
    esp_openclaw_setup_callbacks_t callbacks = {
        .now_ms = now_ms, .peer_is_current_authenticated = authenticated,
        .cancel = cancel, .context = f,
    };
    assert(esp_openclaw_setup_init(&f->session, &callbacks));
    assert(esp_openclaw_setup_transfer_init(&f->transfer, &f->session));
    assert(esp_openclaw_setup_open(&f->session, &f->generation) == ESP_OPENCLAW_SETUP_OK);
    assert(!esp_openclaw_setup_transfer_open(&f->transfer, f->generation, 1));
    assert(esp_openclaw_setup_attach(&f->session, f->generation, 1) == ESP_OPENCLAW_SETUP_OK);
    assert(!esp_openclaw_setup_transfer_open(&f->transfer, f->generation, 2));
    assert(esp_openclaw_setup_transfer_open(&f->transfer, f->generation, 1));
}

static esp_openclaw_setup_frame_t exchange(fixture_t *f, esp_openclaw_setup_frame_t frame)
{
    uint8_t input[ESP_OPENCLAW_SETUP_FRAME_SIZE_MAX];
    size_t size;
    assert(esp_openclaw_setup_frame_encode(&frame, input, sizeof(input), &size));
    assert(esp_openclaw_setup_transfer_receive(&f->transfer, f->generation, 1, input, size,
        f->output, sizeof(f->output), &f->output_size, &f->request));
    esp_openclaw_setup_frame_t reply;
    assert(esp_openclaw_setup_frame_decode(f->output, f->output_size, &reply));
    assert(reply.transfer_id == frame.transfer_id);
    return reply;
}

static esp_openclaw_setup_frame_t fragment(fixture_t *f, uint32_t id, uint16_t total,
    uint16_t offset, const uint8_t *data, uint16_t size)
{
    return exchange(f, (esp_openclaw_setup_frame_t){
        .kind = ESP_OPENCLAW_SETUP_FRAME_REQUEST, .transfer_id = id,
        .total_size = total, .offset = offset, .payload_size = size, .payload = data,
    });
}

static esp_openclaw_setup_frame_t pull(fixture_t *f, uint32_t id, uint16_t offset)
{
    esp_openclaw_setup_frame_t reply = exchange(f, (esp_openclaw_setup_frame_t){
        .kind = ESP_OPENCLAW_SETUP_FRAME_PULL, .transfer_id = id, .offset = offset,
    });
    assert(f->request.data == NULL && f->request.size == 0);
    return reply;
}

static void expect_error(esp_openclaw_setup_frame_t reply, esp_openclaw_setup_frame_error_t error)
{
    assert(reply.kind == ESP_OPENCLAW_SETUP_FRAME_ERROR);
    assert(reply.payload_size == 2 && reply.payload[0] == 0 && reply.payload[1] == error);
}

static void expect_wiped(const esp_openclaw_setup_transfer_t *transfer)
{
    for (size_t i = 0; i < ESP_OPENCLAW_SETUP_MESSAGE_SIZE_MAX; ++i)
        assert(transfer->request[i] == 0 && transfer->response[i] == 0);
}

static void test_reassembly_duplicates_and_pulls(void)
{
    fixture_t f;
    initialize(&f);
    uint8_t message[ESP_OPENCLAW_SETUP_MESSAGE_SIZE_MAX];
    uint8_t response[ESP_OPENCLAW_SETUP_MESSAGE_SIZE_MAX];
    for (size_t i = 0; i < sizeof(message); ++i) {
        message[i] = (uint8_t)(i * 17 + 1);
        response[i] = (uint8_t)(i * 31 + 7);
    }
    esp_openclaw_setup_transfer_token_t token = {0};
    unsigned handoffs = 0;
    for (uint16_t offset = 0; offset < sizeof(message);) {
        uint16_t size = sizeof(message) - offset;
        if (size > 317) size = 317;
        esp_openclaw_setup_frame_t reply = fragment(&f, 3, sizeof(message), offset, message + offset, size);
        assert(reply.kind == ESP_OPENCLAW_SETUP_FRAME_ACK && reply.offset == offset + size);
        assert(reply.total_size == sizeof(message));
        if (f.request.data != NULL) {
            ++handoffs;
            assert(f.request.size == sizeof(message));
            assert(memcmp(f.request.data, message, sizeof(message)) == 0);
            token = f.request.token;
        }
        reply = fragment(&f, 3, sizeof(message), offset, message + offset, size);
        assert(reply.kind == ESP_OPENCLAW_SETUP_FRAME_ACK && reply.offset == offset + size);
        assert(f.request.data == NULL);
        offset += size;
    }
    assert(handoffs == 1 && token.generation == f.generation && token.peer == 1 && token.transfer_id == 3);
    expect_error(pull(&f, 3, 0), ESP_OPENCLAW_SETUP_FRAME_PENDING);
    assert(esp_openclaw_setup_transfer_complete(&f.transfer, token, response, sizeof(response)));
    assert(!esp_openclaw_setup_transfer_complete(&f.transfer, token, message, sizeof(message)));
    uint16_t offsets[] = {8191, 0, 448, 4000, 0};
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        esp_openclaw_setup_frame_t reply = pull(&f, 3, offsets[i]);
        size_t size = sizeof(response) - offsets[i];
        if (size > ESP_OPENCLAW_SETUP_FRAME_PAYLOAD_MAX) size = ESP_OPENCLAW_SETUP_FRAME_PAYLOAD_MAX;
        assert(reply.kind == ESP_OPENCLAW_SETUP_FRAME_RESPONSE && reply.total_size == sizeof(response));
        assert(reply.offset == offsets[i] && reply.payload_size == size);
        assert(memcmp(reply.payload, response + offsets[i], size) == 0);
    }
    esp_openclaw_setup_frame_t reply = fragment(&f, 3, sizeof(message), 8000, message + 8000, 192);
    assert(reply.kind == ESP_OPENCLAW_SETUP_FRAME_ACK && reply.offset == sizeof(message));
    assert(f.request.data == NULL);
    /* A contained duplicate may use different fragment boundaries. */
    reply = fragment(&f, 3, sizeof(message), 10, message + 10, 1);
    assert(reply.kind == ESP_OPENCLAW_SETUP_FRAME_ACK && f.request.data == NULL);

    reply = fragment(&f, 4, 1, 0, message, 1);
    assert(reply.kind == ESP_OPENCLAW_SETUP_FRAME_ACK && f.request.size == 1);
    for (size_t i = 0; i < sizeof(message); ++i) {
        assert(f.transfer.response[i] == 0);
        if (i != 0) assert(f.transfer.request[i] == 0);
    }
    assert(!esp_openclaw_setup_transfer_complete(&f.transfer, token, response, sizeof(response)));
    token = f.request.token;
    assert(esp_openclaw_setup_transfer_complete(&f.transfer, token, response, 1));
    expect_error(pull(&f, 4, 1), ESP_OPENCLAW_SETUP_FRAME_INVALID);
    expect_error(pull(&f, 4, 8191), ESP_OPENCLAW_SETUP_FRAME_INVALID);
    esp_openclaw_setup_transfer_close(&f.transfer);
    expect_wiped(&f.transfer);
}

static void test_conflicts_and_monotonic_ids(void)
{
    fixture_t f;
    initialize(&f);
    const uint8_t data[] = {1, 2, 3, 4, 5, 6};
    expect_error(pull(&f, 1, 0), ESP_OPENCLAW_SETUP_FRAME_STALE);
    fragment(&f, 3, 6, 0, data, 2);
    expect_error(fragment(&f, 4, 6, 0, data, 2), ESP_OPENCLAW_SETUP_FRAME_BUSY);
    expect_error(fragment(&f, 3, 6, 3, data + 3, 1), ESP_OPENCLAW_SETUP_FRAME_INVALID);
    expect_error(fragment(&f, 3, 5, 0, data, 2), ESP_OPENCLAW_SETUP_FRAME_CONFLICT);
    expect_error(fragment(&f, 3, 6, 1, data + 1, 2), ESP_OPENCLAW_SETUP_FRAME_CONFLICT);
    expect_error(fragment(&f, 3, 6, 0, data + 1, 2), ESP_OPENCLAW_SETUP_FRAME_CONFLICT);
    assert(f.request.data == NULL);
    esp_openclaw_setup_frame_t reply = fragment(&f, 3, 6, 2, data + 2, 4);
    assert(reply.kind == ESP_OPENCLAW_SETUP_FRAME_ACK && f.request.size == 6);
    assert(memcmp(f.request.data, data, sizeof(data)) == 0);
    esp_openclaw_setup_transfer_token_t token = f.request.token;
    expect_error(fragment(&f, 4, 6, 0, data, 2), ESP_OPENCLAW_SETUP_FRAME_BUSY);
    assert(esp_openclaw_setup_transfer_complete(&f.transfer, token, data, sizeof(data)));
    expect_error(fragment(&f, 4, 6, 1, data + 1, 1), ESP_OPENCLAW_SETUP_FRAME_INVALID);
    reply = pull(&f, 3, 0);
    assert(reply.kind == ESP_OPENCLAW_SETUP_FRAME_RESPONSE && memcmp(reply.payload, data, sizeof(data)) == 0);
    reply = fragment(&f, 4, 1, 0, data, 1);
    assert(reply.kind == ESP_OPENCLAW_SETUP_FRAME_ACK && f.request.size == 1);
    token = f.request.token;
    expect_error(fragment(&f, 3, 6, 0, data, 2), ESP_OPENCLAW_SETUP_FRAME_STALE);
    expect_error(pull(&f, 3, 0), ESP_OPENCLAW_SETUP_FRAME_STALE);
    expect_error(pull(&f, 5, 0), ESP_OPENCLAW_SETUP_FRAME_STALE);
    assert(esp_openclaw_setup_transfer_complete(&f.transfer, token, data, 1));
    fragment(&f, UINT32_MAX, 1, 0, data, 1);
    token = f.request.token;
    assert(esp_openclaw_setup_transfer_complete(&f.transfer, token, data, 1));
    expect_error(fragment(&f, 1, 1, 0, data, 1), ESP_OPENCLAW_SETUP_FRAME_STALE);
}

static void test_failed_response_is_stable(void)
{
    fixture_t f;
    initialize(&f);
    const uint8_t data[] = {1, 2};
    const size_t sizes[] = {0, 1, ESP_OPENCLAW_SETUP_MESSAGE_SIZE_MAX + 1};
    for (uint32_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        fragment(&f, i + 1, 2, 0, data, 2);
        esp_openclaw_setup_transfer_token_t token = f.request.token;
        const uint8_t *response = i == 1 ? NULL : data;
        assert(!esp_openclaw_setup_transfer_complete(&f.transfer, token, response, sizes[i]));
        assert(!esp_openclaw_setup_transfer_complete(&f.transfer, token, data, 2));
        esp_openclaw_setup_frame_error_t error = i == 2 ?
            ESP_OPENCLAW_SETUP_FRAME_TOO_LARGE : ESP_OPENCLAW_SETUP_FRAME_INTERNAL;
        expect_error(pull(&f, i + 1, 0), error);
        esp_openclaw_setup_frame_t reply = fragment(&f, i + 1, 2, 0, data, 2);
        assert(reply.kind == ESP_OPENCLAW_SETUP_FRAME_ACK && f.request.data == NULL);
        expect_error(pull(&f, i + 1, 0), error);
    }
}

static void reject_unchanged(fixture_t *f, uint64_t generation, uint64_t peer,
    const uint8_t *input, size_t size, size_t capacity)
{
    memset(f->output, 0xa5, sizeof(f->output));
    size_t old_size = f->output_size;
    esp_openclaw_setup_transfer_request_t old_request = f->request;
    assert(!esp_openclaw_setup_transfer_receive(&f->transfer, generation, peer, input, size,
        f->output, capacity, &f->output_size, &f->request));
    assert(f->output_size == old_size && memcmp(&f->request, &old_request, sizeof(old_request)) == 0);
    for (size_t i = 0; i < sizeof(f->output); ++i) assert(f->output[i] == 0xa5);
}

static void test_rejection_preserves_owner_and_retry(void)
{
    fixture_t f;
    initialize(&f);
    const uint8_t data[] = {9, 8};
    uint8_t input[ESP_OPENCLAW_SETUP_FRAME_SIZE_MAX];
    size_t size;
    esp_openclaw_setup_frame_t frame = {
        .kind = ESP_OPENCLAW_SETUP_FRAME_REQUEST, .transfer_id = 1,
        .total_size = 2, .payload_size = 2, .payload = data,
    };
    assert(esp_openclaw_setup_frame_encode(&frame, input, sizeof(input), &size));
    reject_unchanged(&f, f.generation, 1, input, size, sizeof(f.output) - 1);
    reject_unchanged(&f, f.generation, 2, input, size, sizeof(f.output));
    reject_unchanged(&f, f.generation + 1, 1, input, size, sizeof(f.output));
    input[0] = 0;
    reject_unchanged(&f, f.generation, 1, input, size, sizeof(f.output));
    input[0] = 'O';
    esp_openclaw_setup_frame_t reply = exchange(&f, frame);
    assert(reply.kind == ESP_OPENCLAW_SETUP_FRAME_ACK && f.request.size == 2);
    esp_openclaw_setup_transfer_token_t token = f.request.token;
    esp_openclaw_setup_transfer_token_t stranger = token;
    stranger.peer = 2;
    assert(!esp_openclaw_setup_transfer_complete(&f.transfer, stranger, data, 2));
    assert(!esp_openclaw_setup_transfer_open(&f.transfer, f.generation + 1, 2));
    assert(esp_openclaw_setup_transfer_complete(&f.transfer, token, data, 2));
    reject_unchanged(&f, f.generation, 2, input, size, sizeof(f.output));
    reply = pull(&f, 1, 0);
    assert(reply.kind == ESP_OPENCLAW_SETUP_FRAME_RESPONSE && memcmp(reply.payload, data, 2) == 0);
    expect_error(exchange(&f, (esp_openclaw_setup_frame_t){
        .kind = ESP_OPENCLAW_SETUP_FRAME_ACK, .transfer_id = 1, .total_size = 2,
    }), ESP_OPENCLAW_SETUP_FRAME_INVALID);
}

static void test_live_revocation_and_generation_reopen(void)
{
    fixture_t f;
    initialize(&f);
    const uint8_t data[] = {5, 4};
    uint8_t input[ESP_OPENCLAW_SETUP_FRAME_SIZE_MAX];
    size_t size;
    esp_openclaw_setup_frame_t frame = {
        .kind = ESP_OPENCLAW_SETUP_FRAME_REQUEST, .transfer_id = 1,
        .total_size = 2, .payload_size = 2, .payload = data,
    };
    assert(esp_openclaw_setup_frame_encode(&frame, input, sizeof(input), &size));
    f.revoke_after_checks = 1;
    reject_unchanged(&f, f.generation, 1, input, size, sizeof(f.output));
    expect_wiped(&f.transfer);
    assert(!esp_openclaw_setup_transfer_poll(&f.transfer));
    assert(!esp_openclaw_setup_transfer_open(&f.transfer, f.generation, 1));

    initialize(&f);
    fragment(&f, 1, 2, 0, data, 2);
    esp_openclaw_setup_transfer_token_t token = f.request.token;
    f.revoke_after_checks = 1;
    assert(!esp_openclaw_setup_transfer_complete(&f.transfer, token, data, 2));
    expect_wiped(&f.transfer);

    initialize(&f);
    fragment(&f, 1, 2, 0, data, 2);
    assert(esp_openclaw_setup_transfer_complete(&f.transfer, f.request.token, data, 2));
    frame = (esp_openclaw_setup_frame_t){.kind = ESP_OPENCLAW_SETUP_FRAME_PULL, .transfer_id = 1};
    assert(esp_openclaw_setup_frame_encode(&frame, input, sizeof(input), &size));
    f.revoke_after_checks = 1;
    reject_unchanged(&f, f.generation, 1, input, size, sizeof(f.output));
    expect_wiped(&f.transfer);

    initialize(&f);
    fragment(&f, 1, 2, 0, data, 2);
    token = f.request.token;
    assert(esp_openclaw_setup_transfer_complete(&f.transfer, token, data, 2));
    esp_openclaw_setup_transfer_close(&f.transfer);
    expect_wiped(&f.transfer);
    assert(!esp_openclaw_setup_transfer_open(&f.transfer, f.generation, 1));
    esp_openclaw_setup_close(&f.session);
    assert(esp_openclaw_setup_open(&f.session, &f.generation) == ESP_OPENCLAW_SETUP_OK);
    assert(esp_openclaw_setup_attach(&f.session, f.generation, 1) == ESP_OPENCLAW_SETUP_OK);
    assert(esp_openclaw_setup_transfer_open(&f.transfer, f.generation, 1));
    fragment(&f, 1, 2, 0, data, 2);
    assert(!esp_openclaw_setup_transfer_complete(&f.transfer, token, data, 2));
    assert(esp_openclaw_setup_transfer_complete(&f.transfer, f.request.token, data, 2));
    f.now += ESP_OPENCLAW_SETUP_TIMEOUT_MS;
    assert(!esp_openclaw_setup_transfer_poll(&f.transfer));
    expect_wiped(&f.transfer);
}

static void dispatch(esp_openclaw_setup_token_t token, esp_openclaw_setup_operation_t operation,
                     void *context)
{
    (void)token;
    (void)operation;
    ++*(unsigned *)context;
}

static void test_transport_does_not_bypass_confirmation_or_drain(void)
{
    fixture_t f;
    initialize(&f);
    const uint8_t data = 7;
    fragment(&f, 1, 1, 0, &data, 1);
    assert(f.request.size == 1);
    unsigned dispatched = 0;
    esp_openclaw_setup_receipt_t receipt;
    assert(esp_openclaw_setup_submit(&f.session, f.generation, 1, 1, ESP_OPENCLAW_SETUP_WIFI_APPLY,
        dispatch, &dispatched, &receipt) == ESP_OPENCLAW_SETUP_NEEDS_CONFIRMATION);
    assert(dispatched == 0);
    assert(esp_openclaw_setup_get_receipt(&f.session, f.generation, 1, 1, &receipt) ==
        ESP_OPENCLAW_SETUP_NEEDS_CONFIRMATION);
    assert(esp_openclaw_setup_input_released(&f.session, f.generation) == ESP_OPENCLAW_SETUP_OK);
    assert(esp_openclaw_setup_confirm(&f.session, f.generation) == ESP_OPENCLAW_SETUP_OK);
    assert(esp_openclaw_setup_submit(&f.session, f.generation, 1, 1, ESP_OPENCLAW_SETUP_WIFI_APPLY,
        dispatch, &dispatched, &receipt) == ESP_OPENCLAW_SETUP_ACCEPTED);
    assert(dispatched == 1);
    assert(esp_openclaw_setup_transfer_complete(&f.transfer, f.request.token, &data, 1));
    fragment(&f, 2, 1, 0, &data, 1);
    assert(esp_openclaw_setup_submit(&f.session, f.generation, 1, 1, ESP_OPENCLAW_SETUP_WIFI_APPLY,
        dispatch, &dispatched, &receipt) == ESP_OPENCLAW_SETUP_DUPLICATE);
    assert(dispatched == 1);
    f.authenticated = false;
    assert(!esp_openclaw_setup_transfer_poll(&f.transfer));
    expect_wiped(&f.transfer);
    assert(f.canceled == 1);
    assert(esp_openclaw_setup_snapshot(&f.session).phase == ESP_OPENCLAW_SETUP_CLOSING);
    assert(esp_openclaw_setup_complete(&f.session, receipt.token, ESP_OPENCLAW_SETUP_UNKNOWN, 0));
}

int main(void)
{
    test_reassembly_duplicates_and_pulls();
    test_conflicts_and_monotonic_ids();
    test_failed_response_is_stable();
    test_rejection_preserves_owner_and_retry();
    test_live_revocation_and_generation_reopen();
    test_transport_does_not_bypass_confirmation_or_drain();
    puts("setup transfer tests passed");
    return 0;
}
