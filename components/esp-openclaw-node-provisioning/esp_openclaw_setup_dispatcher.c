#include "esp_openclaw_setup_dispatcher.h"

#include <string.h>

static void wipe(esp_openclaw_setup_dispatcher_t *dispatch)
{
    volatile uint8_t *bytes = (volatile uint8_t *)&dispatch->scratch;
    for (size_t i = 0; i < sizeof(dispatch->scratch); ++i) bytes[i] = 0;
}

bool esp_openclaw_setup_dispatcher_init(esp_openclaw_setup_dispatcher_t *dispatch,
    esp_openclaw_setup_session_t *session, esp_openclaw_setup_transfer_t *transfer,
    esp_openclaw_setup_wifi_t *wifi, esp_openclaw_setup_gateway_t *gateway,
    esp_openclaw_setup_hello_t hello, void *context)
{
    if (!dispatch || !session || !transfer || !wifi || !gateway || !hello) return false;
    memset(dispatch, 0, sizeof(*dispatch));
    dispatch->session = session; dispatch->transfer = transfer;
    dispatch->wifi = wifi; dispatch->gateway = gateway;
    dispatch->hello = hello; dispatch->context = context;
    return true;
}

esp_err_t esp_openclaw_setup_dispatcher_activate(esp_openclaw_setup_dispatcher_t *dispatch, uint64_t generation)
{
    esp_err_t err = esp_openclaw_setup_wifi_activate(dispatch->wifi, generation);
    return err == ESP_OK ? esp_openclaw_setup_gateway_activate(dispatch->gateway, generation) : err;
}

void esp_openclaw_setup_dispatcher_cancel(esp_openclaw_setup_dispatcher_t *dispatch, esp_openclaw_setup_token_t token)
{
    esp_openclaw_setup_wifi_cancel(dispatch->wifi, token);
    esp_openclaw_setup_gateway_cancel(dispatch->gateway, token);
}

esp_err_t esp_openclaw_setup_dispatcher_poll(esp_openclaw_setup_dispatcher_t *dispatch, bool *gateway_released)
{
    if (gateway_released == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t wifi = esp_openclaw_setup_wifi_poll(dispatch->wifi);
    esp_err_t gateway = esp_openclaw_setup_gateway_poll(dispatch->gateway, gateway_released);
    esp_openclaw_setup_transfer_poll(dispatch->transfer);
    return wifi != ESP_OK ? wifi : gateway;
}

static esp_openclaw_setup_response_code_t session_code(esp_openclaw_setup_result_t result)
{
    switch (result) {
    case ESP_OPENCLAW_SETUP_OK: return ESP_OPENCLAW_SETUP_RESPONSE_OK;
    case ESP_OPENCLAW_SETUP_ACCEPTED: return ESP_OPENCLAW_SETUP_RESPONSE_ACCEPTED;
    case ESP_OPENCLAW_SETUP_DUPLICATE: return ESP_OPENCLAW_SETUP_RESPONSE_DUPLICATE;
    case ESP_OPENCLAW_SETUP_INVALID: return ESP_OPENCLAW_SETUP_RESPONSE_INVALID;
    case ESP_OPENCLAW_SETUP_STALE: return ESP_OPENCLAW_SETUP_RESPONSE_STALE;
    case ESP_OPENCLAW_SETUP_NEEDS_CONFIRMATION: return ESP_OPENCLAW_SETUP_RESPONSE_CONFIRM;
    case ESP_OPENCLAW_SETUP_OCCUPIED: return ESP_OPENCLAW_SETUP_RESPONSE_BUSY;
    case ESP_OPENCLAW_SETUP_FULL: return ESP_OPENCLAW_SETUP_RESPONSE_FULL;
    case ESP_OPENCLAW_SETUP_NOT_FOUND: return ESP_OPENCLAW_SETUP_RESPONSE_NOT_FOUND;
    case ESP_OPENCLAW_SETUP_UNAUTHORIZED: return ESP_OPENCLAW_SETUP_RESPONSE_UNAVAILABLE;
    default: return ESP_OPENCLAW_SETUP_RESPONSE_INTERNAL;
    }
}

static esp_openclaw_setup_response_code_t native_code(esp_err_t error)
{
    switch (error) {
    case ESP_OK: return ESP_OPENCLAW_SETUP_RESPONSE_OK;
    case ESP_ERR_INVALID_ARG: return ESP_OPENCLAW_SETUP_RESPONSE_INVALID;
    case ESP_ERR_WIFI_STATE: return ESP_OPENCLAW_SETUP_RESPONSE_BUSY;
    case ESP_ERR_NOT_SUPPORTED: return ESP_OPENCLAW_SETUP_RESPONSE_UNSUPPORTED;
    default: return ESP_OPENCLAW_SETUP_RESPONSE_UNAVAILABLE;
    }
}

static esp_openclaw_setup_response_phase_t phase(esp_openclaw_setup_phase_t value)
{
    switch (value) {
    case ESP_OPENCLAW_SETUP_COMPARISON: return ESP_OPENCLAW_SETUP_RESPONSE_COMPARISON;
    case ESP_OPENCLAW_SETUP_READY: return ESP_OPENCLAW_SETUP_RESPONSE_READY;
    case ESP_OPENCLAW_SETUP_BUSY: return ESP_OPENCLAW_SETUP_RESPONSE_WORKING;
    default: return 0; /* A revoked session cannot encode or publish saved state. */
    }
}

static bool token_matches(esp_openclaw_setup_token_t a, esp_openclaw_setup_token_t b)
{ return a.generation == b.generation && a.operation_id == b.operation_id; }

static esp_openclaw_setup_response_detail_t detail(int32_t value)
{
    switch (value) {
    case ESP_OPENCLAW_SETUP_WIFI_CANCELLED:
    case ESP_OPENCLAW_SETUP_GATEWAY_CANCELLED: return ESP_OPENCLAW_SETUP_RESPONSE_DETAIL_CANCELLED;
    case ESP_ERR_INVALID_ARG: return ESP_OPENCLAW_SETUP_RESPONSE_DETAIL_INPUT;
    case ESP_ERR_NOT_SUPPORTED: return ESP_OPENCLAW_SETUP_RESPONSE_DETAIL_UNSUPPORTED;
    case ESP_ERR_WIFI_STATE: return ESP_OPENCLAW_SETUP_RESPONSE_DETAIL_BUSY;
    case ESP_ERR_TIMEOUT: return ESP_OPENCLAW_SETUP_RESPONSE_DETAIL_TIMEOUT;
    case ESP_ERR_NO_MEM: return ESP_OPENCLAW_SETUP_RESPONSE_DETAIL_RESOURCE;
    default: return ESP_OPENCLAW_SETUP_RESPONSE_DETAIL_FAILED;
    }
}

static void receipt_body(esp_openclaw_setup_dispatcher_t *dispatch,
    const esp_openclaw_setup_receipt_t *receipt, esp_openclaw_setup_snapshot_t snapshot)
{
    esp_openclaw_setup_response_status_t *body = &dispatch->response.body.status;
    body->phase = phase(snapshot.phase); body->receipt_count = snapshot.receipt_count;
    if (receipt == NULL) return;
    body->operation_id = receipt->token.operation_id;
    body->operation = receipt->operation == ESP_OPENCLAW_SETUP_WIFI_APPLY ?
        ESP_OPENCLAW_SETUP_REQUEST_WIFI_APPLY : ESP_OPENCLAW_SETUP_REQUEST_GATEWAY_APPLY;
    switch (receipt->outcome) {
    case ESP_OPENCLAW_SETUP_PENDING: body->outcome = ESP_OPENCLAW_SETUP_RESPONSE_PENDING; break;
    case ESP_OPENCLAW_SETUP_SUCCEEDED: body->outcome = ESP_OPENCLAW_SETUP_RESPONSE_SUCCEEDED; break;
    case ESP_OPENCLAW_SETUP_FAILED:
        body->outcome = ESP_OPENCLAW_SETUP_RESPONSE_FAILED; body->detail = detail(receipt->detail); break;
    case ESP_OPENCLAW_SETUP_UNKNOWN: body->outcome = ESP_OPENCLAW_SETUP_RESPONSE_UNKNOWN; break;
    }
    bool observed = false, saved = false;
    if (receipt->operation == ESP_OPENCLAW_SETUP_WIFI_APPLY) {
        esp_openclaw_setup_wifi_snapshot_t wifi = esp_openclaw_setup_wifi_snapshot(dispatch->wifi);
        observed = wifi.attempt.id != 0 && token_matches(receipt->token, wifi.token);
        if (observed) {
            saved = wifi.attempt.credentials_saved;
            switch (wifi.attempt.state) {
            case ESP_OPENCLAW_NODE_WIFI_PREPARING: body->progress = ESP_OPENCLAW_SETUP_RESPONSE_PREPARING; break;
            case ESP_OPENCLAW_NODE_WIFI_JOINING: body->progress = ESP_OPENCLAW_SETUP_RESPONSE_CONNECTING; break;
            case ESP_OPENCLAW_NODE_WIFI_DRAINING: body->progress = ESP_OPENCLAW_SETUP_RESPONSE_DRAINING; break;
            case ESP_OPENCLAW_NODE_WIFI_RECOVERY_REQUIRED: body->progress = ESP_OPENCLAW_SETUP_RESPONSE_RECOVERY_REQUIRED; break;
            default: body->progress = ESP_OPENCLAW_SETUP_RESPONSE_TERMINAL; break;
            }
        }
    } else {
        esp_openclaw_setup_gateway_snapshot_t gateway = esp_openclaw_setup_gateway_snapshot(dispatch->gateway);
        observed = gateway.attempt.id != 0 && token_matches(receipt->token, gateway.token);
        if (observed) {
            saved = gateway.attempt.credentials_saved;
            switch (gateway.attempt.state) {
            case ESP_OPENCLAW_NODE_CONNECT_ATTEMPT_QUEUED: body->progress = ESP_OPENCLAW_SETUP_RESPONSE_PREPARING; break;
            case ESP_OPENCLAW_NODE_CONNECT_ATTEMPT_CONNECTING: body->progress = ESP_OPENCLAW_SETUP_RESPONSE_CONNECTING; break;
            default: body->progress = ESP_OPENCLAW_SETUP_RESPONSE_TERMINAL; break;
            }
        }
    }
    if (observed) {
        body->effects_known = ESP_OPENCLAW_SETUP_RESPONSE_SAVE_CONFIRMED;
        body->effects_value = saved ? ESP_OPENCLAW_SETUP_RESPONSE_SAVE_CONFIRMED : 0;
    }
}

static void scan_body(esp_openclaw_setup_dispatcher_t *dispatch, uint64_t peer)
{
    uint16_t count = ESP_OPENCLAW_SETUP_RESPONSE_SCAN_MAX, total = 0;
    esp_err_t err = esp_openclaw_setup_wifi_scan(dispatch->wifi, dispatch->response.generation,
        peer, dispatch->scratch.scan, &count, &total);
    dispatch->response.code = native_code(err);
    if (err != ESP_OK) return;
    dispatch->response.body.scan.count = (uint8_t)count;
    dispatch->response.body.scan.total = total;
    for (unsigned i = 0; i < count; ++i) {
        const wifi_ap_record_t *ap = &dispatch->scratch.scan[i];
        esp_openclaw_setup_response_network_t *network = &dispatch->response.body.scan.networks[i];
        network->ssid_size = (uint8_t)strnlen((const char *)ap->ssid, 32);
        memcpy(network->ssid, ap->ssid, network->ssid_size);
        network->rssi = ap->rssi; network->channel = ap->primary;
        switch (ap->authmode) {
        case WIFI_AUTH_OPEN: network->security = ESP_OPENCLAW_SETUP_RESPONSE_NETWORK_OPEN; break;
        case WIFI_AUTH_WPA_PSK: case WIFI_AUTH_WPA2_PSK: case WIFI_AUTH_WPA_WPA2_PSK:
        case WIFI_AUTH_WPA3_PSK: case WIFI_AUTH_WPA2_WPA3_PSK:
            network->security = ESP_OPENCLAW_SETUP_RESPONSE_NETWORK_PASSWORD; break;
        default: network->security = ESP_OPENCLAW_SETUP_RESPONSE_NETWORK_UNSUPPORTED; break;
        }
    }
}

bool esp_openclaw_setup_dispatcher_handle(esp_openclaw_setup_dispatcher_t *dispatch,
    const esp_openclaw_setup_transfer_request_t *handoff)
{
    if (!esp_openclaw_setup_transfer_authorize_request(dispatch->transfer, handoff)) return false;
    dispatch->response = (esp_openclaw_setup_response_t){0};
    esp_openclaw_setup_response_t *response = &dispatch->response;
    esp_openclaw_setup_request_t request;
    esp_openclaw_setup_request_result_t decoded = esp_openclaw_setup_request_decode(handoff->data, handoff->size, &request);
    if (decoded != ESP_OPENCLAW_SETUP_REQUEST_OK) {
        switch (decoded) {
        case ESP_OPENCLAW_SETUP_REQUEST_UNSUPPORTED_VERSION: response->code = ESP_OPENCLAW_SETUP_RESPONSE_VERSION; break;
        case ESP_OPENCLAW_SETUP_REQUEST_UNKNOWN_OPERATION: response->code = ESP_OPENCLAW_SETUP_RESPONSE_OPERATION; break;
        case ESP_OPENCLAW_SETUP_REQUEST_TOO_LARGE: response->code = ESP_OPENCLAW_SETUP_RESPONSE_TOO_LARGE; break;
        default: response->code = ESP_OPENCLAW_SETUP_RESPONSE_INVALID; break;
        }
    } else {
        response->operation = request.kind; response->generation = request.generation;
        response->operation_id = request.operation_id;
        uint64_t generation = request.kind == ESP_OPENCLAW_SETUP_REQUEST_HELLO ? handoff->token.generation : request.generation;
        esp_openclaw_setup_result_t authorized = esp_openclaw_setup_authorize_peer(dispatch->session, generation, handoff->token.peer);
        esp_openclaw_setup_snapshot_t snapshot = esp_openclaw_setup_snapshot(dispatch->session);
        if (authorized == ESP_OPENCLAW_SETUP_OK && request.kind != ESP_OPENCLAW_SETUP_REQUEST_HELLO &&
            request.kind != ESP_OPENCLAW_SETUP_REQUEST_CANCEL && snapshot.phase == ESP_OPENCLAW_SETUP_COMPARISON)
            authorized = ESP_OPENCLAW_SETUP_NEEDS_CONFIRMATION;
        response->code = session_code(authorized);
        if (authorized == ESP_OPENCLAW_SETUP_OK) {
            esp_openclaw_setup_receipt_t receipt;
            switch (request.kind) {
            case ESP_OPENCLAW_SETUP_REQUEST_HELLO:
                if (dispatch->hello(generation, handoff->token.peer, &response->body.hello.label,
                        &response->body.hello.model, &response->body.hello.challenge, dispatch->context)) {
                    response->generation = generation; response->body.hello.phase = phase(snapshot.phase);
                } else response->code = ESP_OPENCLAW_SETUP_RESPONSE_UNAVAILABLE;
                break;
            case ESP_OPENCLAW_SETUP_REQUEST_CANCEL:
                esp_openclaw_setup_close(dispatch->session);
                esp_openclaw_setup_transfer_poll(dispatch->transfer);
                return false;
            case ESP_OPENCLAW_SETUP_REQUEST_STATUS:
                if (request.operation_id != 0) {
                    response->code = session_code(esp_openclaw_setup_get_receipt(dispatch->session,
                        generation, handoff->token.peer, request.operation_id, &receipt));
                    if (response->code == ESP_OPENCLAW_SETUP_RESPONSE_OK) receipt_body(dispatch, &receipt, snapshot);
                } else receipt_body(dispatch, snapshot.has_operation ? &snapshot.operation : NULL, snapshot);
                break;
            case ESP_OPENCLAW_SETUP_REQUEST_WIFI_SCAN:
                if (snapshot.phase == ESP_OPENCLAW_SETUP_BUSY) response->code = ESP_OPENCLAW_SETUP_RESPONSE_BUSY;
                else scan_body(dispatch, handoff->token.peer);
                break;
            case ESP_OPENCLAW_SETUP_REQUEST_WIFI_APPLY:
                memcpy(dispatch->scratch.wifi.ssid, request.payload.wifi.ssid.data, request.payload.wifi.ssid.size);
                dispatch->scratch.wifi.ssid[request.payload.wifi.ssid.size] = 0;
                memcpy(dispatch->scratch.wifi.password, request.payload.wifi.password.data, request.payload.wifi.password.size);
                dispatch->scratch.wifi.password[request.payload.wifi.password.size] = 0;
                response->code = session_code(esp_openclaw_setup_wifi_submit(dispatch->wifi, generation,
                    handoff->token.peer, request.operation_id, dispatch->scratch.wifi.ssid,
                    dispatch->scratch.wifi.password, &receipt));
                break;
            case ESP_OPENCLAW_SETUP_REQUEST_GATEWAY_APPLY:
                memcpy(dispatch->scratch.setup_code, request.payload.setup_code.data, request.payload.setup_code.size);
                dispatch->scratch.setup_code[request.payload.setup_code.size] = 0;
                response->code = session_code(esp_openclaw_setup_gateway_submit(dispatch->gateway, generation,
                    handoff->token.peer, request.operation_id, dispatch->scratch.setup_code, &receipt));
                break;
            }
            if (response->code == ESP_OPENCLAW_SETUP_RESPONSE_ACCEPTED || response->code == ESP_OPENCLAW_SETUP_RESPONSE_DUPLICATE)
                receipt_body(dispatch, &receipt, esp_openclaw_setup_snapshot(dispatch->session));
        }
    }
    wipe(dispatch);
    size_t size = 0;
    bool encoded = esp_openclaw_setup_response_encode(response, dispatch->encoded, sizeof(dispatch->encoded), &size);
    bool published = esp_openclaw_setup_transfer_complete(dispatch->transfer, handoff->token,
        encoded ? dispatch->encoded : NULL, size);
    dispatch->response = (esp_openclaw_setup_response_t){0};
    return published;
}
