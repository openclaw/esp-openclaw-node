#include "esp_openclaw_setup_session.h"

#include <stddef.h>
#include <string.h>

static void finish_close(esp_openclaw_setup_session_t *session)
{
    session->phase = ESP_OPENCLAW_SETUP_CLOSED;
    session->peer = 0;
    session->input_released = false;
    session->receipt_count = 0;
    memset(session->receipts, 0, sizeof(session->receipts));
}

static bool live(esp_openclaw_setup_session_t *session)
{
    uint64_t now = session->callbacks.now_ms(session->callbacks.context);
    return now >= session->opened_ms && now - session->opened_ms < ESP_OPENCLAW_SETUP_TIMEOUT_MS &&
        (session->peer == 0 || session->callbacks.peer_is_current_authenticated(
            session->peer, session->callbacks.context));
}

bool esp_openclaw_setup_init(esp_openclaw_setup_session_t *session,
                             const esp_openclaw_setup_callbacks_t *callbacks)
{
    if (session == NULL || callbacks == NULL || callbacks->now_ms == NULL ||
        callbacks->peer_is_current_authenticated == NULL || callbacks->cancel == NULL) return false;
    *session = (esp_openclaw_setup_session_t){.callbacks = *callbacks};
    return true;
}

void esp_openclaw_setup_close(esp_openclaw_setup_session_t *session)
{
    if (session->phase == ESP_OPENCLAW_SETUP_CLOSED || session->phase == ESP_OPENCLAW_SETUP_CLOSING) return;
    if (session->phase == ESP_OPENCLAW_SETUP_BUSY) {
        esp_openclaw_setup_token_t token = session->receipts[session->receipt_count - 1].token;
        session->phase = ESP_OPENCLAW_SETUP_CLOSING;
        session->peer = 0;
        session->input_released = false;
        session->callbacks.cancel(token, session->callbacks.context);
    } else {
        finish_close(session);
    }
}

void esp_openclaw_setup_poll(esp_openclaw_setup_session_t *session)
{
    if (session->phase != ESP_OPENCLAW_SETUP_CLOSED && session->phase != ESP_OPENCLAW_SETUP_CLOSING &&
        !live(session)) esp_openclaw_setup_close(session);
}

bool esp_openclaw_setup_detach(esp_openclaw_setup_session_t *session,
                              uint64_t generation, uint64_t peer)
{
    if (peer == 0 || peer != session->peer || generation != session->generation) return false;
    esp_openclaw_setup_close(session);
    return true;
}

esp_openclaw_setup_result_t esp_openclaw_setup_open(esp_openclaw_setup_session_t *session,
                                                   uint64_t *generation)
{
    if (generation == NULL) return ESP_OPENCLAW_SETUP_INVALID;
    esp_openclaw_setup_poll(session);
    if (session->phase != ESP_OPENCLAW_SETUP_CLOSED) return ESP_OPENCLAW_SETUP_OCCUPIED;
    if (session->generation == UINT64_MAX) return ESP_OPENCLAW_SETUP_EXHAUSTED;
    session->generation++;
    session->opened_ms = session->callbacks.now_ms(session->callbacks.context);
    session->phase = ESP_OPENCLAW_SETUP_DISCOVERABLE;
    *generation = session->generation;
    return ESP_OPENCLAW_SETUP_OK;
}

esp_openclaw_setup_result_t esp_openclaw_setup_attach(esp_openclaw_setup_session_t *session,
                                                     uint64_t generation, uint64_t peer)
{
    if (peer == 0) return ESP_OPENCLAW_SETUP_INVALID;
    esp_openclaw_setup_poll(session);
    if (generation != session->generation || session->phase == ESP_OPENCLAW_SETUP_CLOSED ||
        session->phase == ESP_OPENCLAW_SETUP_CLOSING) return ESP_OPENCLAW_SETUP_STALE;
    if (session->phase != ESP_OPENCLAW_SETUP_DISCOVERABLE) return ESP_OPENCLAW_SETUP_OCCUPIED;
    if (!session->callbacks.peer_is_current_authenticated(peer, session->callbacks.context))
        return ESP_OPENCLAW_SETUP_UNAUTHORIZED;
    session->peer = peer;
    session->phase = ESP_OPENCLAW_SETUP_COMPARISON;
    return ESP_OPENCLAW_SETUP_OK;
}

static bool comparison_is_current(esp_openclaw_setup_session_t *session, uint64_t generation)
{
    esp_openclaw_setup_poll(session);
    return generation == session->generation && session->phase == ESP_OPENCLAW_SETUP_COMPARISON;
}

esp_openclaw_setup_result_t esp_openclaw_setup_input_released(esp_openclaw_setup_session_t *session,
                                                             uint64_t generation)
{
    if (!comparison_is_current(session, generation)) return ESP_OPENCLAW_SETUP_STALE;
    session->input_released = true;
    return ESP_OPENCLAW_SETUP_OK;
}

esp_openclaw_setup_result_t esp_openclaw_setup_confirm(esp_openclaw_setup_session_t *session,
                                                      uint64_t generation)
{
    if (!comparison_is_current(session, generation)) return ESP_OPENCLAW_SETUP_STALE;
    if (!session->input_released) return ESP_OPENCLAW_SETUP_NEEDS_CONFIRMATION;
    session->phase = ESP_OPENCLAW_SETUP_READY;
    session->input_released = false;
    return ESP_OPENCLAW_SETUP_OK;
}

esp_openclaw_setup_result_t esp_openclaw_setup_authorize_peer(esp_openclaw_setup_session_t *session,
                                                             uint64_t generation, uint64_t peer)
{
    esp_openclaw_setup_poll(session);
    if (generation != session->generation || session->phase == ESP_OPENCLAW_SETUP_CLOSED ||
        session->phase == ESP_OPENCLAW_SETUP_CLOSING) return ESP_OPENCLAW_SETUP_STALE;
    if (peer == 0 || peer != session->peer) return ESP_OPENCLAW_SETUP_UNAUTHORIZED;
    return ESP_OPENCLAW_SETUP_OK;
}

static esp_openclaw_setup_result_t authorize_peer(esp_openclaw_setup_session_t *session,
                                                 uint64_t generation, uint64_t peer)
{
    esp_openclaw_setup_result_t result = esp_openclaw_setup_authorize_peer(session, generation, peer);
    if (result != ESP_OPENCLAW_SETUP_OK) return result;
    if (session->phase != ESP_OPENCLAW_SETUP_READY && session->phase != ESP_OPENCLAW_SETUP_BUSY)
        return ESP_OPENCLAW_SETUP_NEEDS_CONFIRMATION;
    return ESP_OPENCLAW_SETUP_OK;
}

static esp_openclaw_setup_receipt_t *find_receipt(esp_openclaw_setup_session_t *session, uint32_t id)
{
    for (uint8_t i = 0; i < session->receipt_count; ++i) {
        if (session->receipts[i].token.operation_id == id) return &session->receipts[i];
    }
    return NULL;
}

esp_openclaw_setup_result_t esp_openclaw_setup_get_receipt(esp_openclaw_setup_session_t *session,
    uint64_t generation, uint64_t peer, uint32_t operation_id, esp_openclaw_setup_receipt_t *receipt)
{
    if (operation_id == 0 || receipt == NULL) return ESP_OPENCLAW_SETUP_INVALID;
    esp_openclaw_setup_result_t result = authorize_peer(session, generation, peer);
    if (result != ESP_OPENCLAW_SETUP_OK) return result;
    esp_openclaw_setup_receipt_t *found = find_receipt(session, operation_id);
    if (found == NULL) return ESP_OPENCLAW_SETUP_NOT_FOUND;
    *receipt = *found;
    return ESP_OPENCLAW_SETUP_OK;
}

esp_openclaw_setup_result_t esp_openclaw_setup_submit(esp_openclaw_setup_session_t *session,
    uint64_t generation, uint64_t peer, uint32_t operation_id,
    esp_openclaw_setup_operation_t operation, esp_openclaw_setup_dispatch_t dispatch,
    void *payload_context, esp_openclaw_setup_receipt_t *receipt)
{
    if (operation_id == 0 || dispatch == NULL || receipt == NULL ||
        (operation != ESP_OPENCLAW_SETUP_WIFI_APPLY && operation != ESP_OPENCLAW_SETUP_GATEWAY_APPLY))
        return ESP_OPENCLAW_SETUP_INVALID;
    esp_openclaw_setup_result_t result = authorize_peer(session, generation, peer);
    if (result != ESP_OPENCLAW_SETUP_OK) return result;
    esp_openclaw_setup_receipt_t *found = find_receipt(session, operation_id);
    if (found != NULL) {
        if (found->operation != operation) return ESP_OPENCLAW_SETUP_INVALID;
        *receipt = *found;
        return ESP_OPENCLAW_SETUP_DUPLICATE;
    }
    if (session->phase == ESP_OPENCLAW_SETUP_BUSY) return ESP_OPENCLAW_SETUP_OCCUPIED;
    if (session->receipt_count == ESP_OPENCLAW_SETUP_RECEIPT_LIMIT) return ESP_OPENCLAW_SETUP_FULL;
    /* Recheck immediately before dispatch; receipt/token equality is not authority. */
    result = authorize_peer(session, generation, peer);
    if (result != ESP_OPENCLAW_SETUP_OK) return result;
    *receipt = (esp_openclaw_setup_receipt_t){
        .token = {.generation = generation, .operation_id = operation_id},
        .operation = operation, .outcome = ESP_OPENCLAW_SETUP_PENDING,
    };
    session->receipts[session->receipt_count++] = *receipt;
    session->phase = ESP_OPENCLAW_SETUP_BUSY;
    dispatch(receipt->token, operation, payload_context);
    return ESP_OPENCLAW_SETUP_ACCEPTED;
}

static bool pending_matches(esp_openclaw_setup_session_t *session, esp_openclaw_setup_token_t token)
{
    if ((session->phase != ESP_OPENCLAW_SETUP_BUSY && session->phase != ESP_OPENCLAW_SETUP_CLOSING) ||
        token.generation != session->generation || session->receipt_count == 0) return false;
    return token.operation_id == session->receipts[session->receipt_count - 1].token.operation_id;
}

bool esp_openclaw_setup_authorize_operation(esp_openclaw_setup_session_t *session,
                                           esp_openclaw_setup_token_t token)
{
    esp_openclaw_setup_poll(session);
    return session->phase == ESP_OPENCLAW_SETUP_BUSY && pending_matches(session, token);
}

bool esp_openclaw_setup_complete(esp_openclaw_setup_session_t *session,
    esp_openclaw_setup_token_t token, esp_openclaw_setup_outcome_t outcome, int32_t detail)
{
    if (outcome != ESP_OPENCLAW_SETUP_SUCCEEDED && outcome != ESP_OPENCLAW_SETUP_FAILED &&
        outcome != ESP_OPENCLAW_SETUP_UNKNOWN) return false;
    if (!pending_matches(session, token)) return false;
    /* The worker is already drained: loss of authority needs no second cancel. */
    if (session->phase == ESP_OPENCLAW_SETUP_CLOSING || !live(session)) {
        finish_close(session);
    } else {
        esp_openclaw_setup_receipt_t *receipt = &session->receipts[session->receipt_count - 1];
        receipt->outcome = outcome;
        receipt->detail = detail;
        session->phase = ESP_OPENCLAW_SETUP_READY;
    }
    return true;
}

esp_openclaw_setup_snapshot_t esp_openclaw_setup_snapshot(esp_openclaw_setup_session_t *session)
{
    esp_openclaw_setup_poll(session);
    esp_openclaw_setup_snapshot_t snapshot = {
        .generation = session->generation, .phase = session->phase,
        .receipt_count = session->receipt_count, .has_operation = session->receipt_count != 0,
    };
    if (snapshot.has_operation) snapshot.operation = session->receipts[session->receipt_count - 1];
    return snapshot;
}
