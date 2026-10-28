#include "esp_openclaw_setup_session.h"

#include <assert.h>
#include <stdio.h>

typedef struct {
    esp_openclaw_setup_session_t session;
    uint64_t now, generation;
    bool authenticated, revoke_after_check, complete_in_dispatch, complete_in_cancel;
    unsigned dispatched, canceled, applied;
    esp_openclaw_setup_token_t token;
} fixture_t;

static uint64_t now_ms(void *context) { return ((fixture_t *)context)->now; }

static bool authenticated(uint64_t peer, void *context)
{
    fixture_t *f = context;
    bool result = f->authenticated && (peer == 1 || peer == 2);
    if (f->revoke_after_check) f->authenticated = false;
    return result;
}

static void cancel(esp_openclaw_setup_token_t token, void *context)
{
    fixture_t *f = context;
    ++f->canceled;
    assert(token.generation == f->token.generation && token.operation_id == f->token.operation_id);
    if (f->complete_in_cancel)
        assert(esp_openclaw_setup_complete(&f->session, token, ESP_OPENCLAW_SETUP_UNKNOWN, 0));
}

static void dispatch(esp_openclaw_setup_token_t token, esp_openclaw_setup_operation_t operation,
                     void *context)
{
    fixture_t *f = context;
    assert(operation == ESP_OPENCLAW_SETUP_WIFI_APPLY || operation == ESP_OPENCLAW_SETUP_GATEWAY_APPLY);
    ++f->dispatched;
    f->token = token;
    if (f->complete_in_dispatch) {
        assert(esp_openclaw_setup_authorize_operation(&f->session, token));
        ++f->applied;
        assert(esp_openclaw_setup_complete(&f->session, token, ESP_OPENCLAW_SETUP_FAILED, 17));
    }
}

static void initialize(fixture_t *f)
{
    *f = (fixture_t){.now = 100, .authenticated = true};
    esp_openclaw_setup_callbacks_t callbacks = {
        .now_ms = now_ms, .peer_is_current_authenticated = authenticated,
        .cancel = cancel, .context = f,
    };
    assert(esp_openclaw_setup_init(&f->session, &callbacks));
    assert(esp_openclaw_setup_open(&f->session, &f->generation) == ESP_OPENCLAW_SETUP_OK);
}

static void confirm(fixture_t *f)
{
    assert(esp_openclaw_setup_attach(&f->session, f->generation, 1) == ESP_OPENCLAW_SETUP_OK);
    assert(esp_openclaw_setup_input_released(&f->session, f->generation) == ESP_OPENCLAW_SETUP_OK);
    assert(esp_openclaw_setup_confirm(&f->session, f->generation) == ESP_OPENCLAW_SETUP_OK);
}

static esp_openclaw_setup_result_t submit(fixture_t *f, uint32_t id,
                                         esp_openclaw_setup_receipt_t *receipt)
{
    return esp_openclaw_setup_submit(&f->session, f->generation, 1, id,
        ESP_OPENCLAW_SETUP_WIFI_APPLY, dispatch, f, receipt);
}

static void test_local_confirmation_and_peer_ownership(void)
{
    fixture_t f;
    initialize(&f);
    esp_openclaw_setup_receipt_t receipt;
    assert(submit(&f, 1, &receipt) == ESP_OPENCLAW_SETUP_UNAUTHORIZED);
    assert(esp_openclaw_setup_confirm(&f.session, f.generation) == ESP_OPENCLAW_SETUP_STALE);
    f.authenticated = false;
    assert(esp_openclaw_setup_attach(&f.session, f.generation, 1) == ESP_OPENCLAW_SETUP_UNAUTHORIZED);
    f.authenticated = true;
    assert(esp_openclaw_setup_attach(&f.session, f.generation, 1) == ESP_OPENCLAW_SETUP_OK);
    assert(esp_openclaw_setup_attach(&f.session, f.generation, 2) == ESP_OPENCLAW_SETUP_OCCUPIED);
    assert(submit(&f, 1, &receipt) == ESP_OPENCLAW_SETUP_NEEDS_CONFIRMATION);
    assert(esp_openclaw_setup_confirm(&f.session, f.generation) == ESP_OPENCLAW_SETUP_NEEDS_CONFIRMATION);
    assert(esp_openclaw_setup_input_released(&f.session, f.generation + 1) == ESP_OPENCLAW_SETUP_STALE);
    assert(esp_openclaw_setup_input_released(&f.session, f.generation) == ESP_OPENCLAW_SETUP_OK);
    assert(esp_openclaw_setup_confirm(&f.session, f.generation) == ESP_OPENCLAW_SETUP_OK);
    assert(esp_openclaw_setup_submit(&f.session, f.generation, 2, 1, ESP_OPENCLAW_SETUP_WIFI_APPLY,
        dispatch, &f, &receipt) == ESP_OPENCLAW_SETUP_UNAUTHORIZED);
    assert(!esp_openclaw_setup_detach(&f.session, f.generation, 2));
    assert(f.dispatched == 0);
    assert(submit(&f, 1, &receipt) == ESP_OPENCLAW_SETUP_ACCEPTED);
    assert(esp_openclaw_setup_authorize_operation(&f.session, receipt.token));
}

static void test_receipts_and_single_pending_operation(void)
{
    fixture_t f;
    initialize(&f);
    confirm(&f);
    esp_openclaw_setup_receipt_t receipt;
    assert(submit(&f, 0, &receipt) == ESP_OPENCLAW_SETUP_INVALID);
    assert(submit(&f, 1, &receipt) == ESP_OPENCLAW_SETUP_ACCEPTED);
    assert(receipt.outcome == ESP_OPENCLAW_SETUP_PENDING);
    assert(submit(&f, 1, &receipt) == ESP_OPENCLAW_SETUP_DUPLICATE);
    assert(submit(&f, 2, &receipt) == ESP_OPENCLAW_SETUP_OCCUPIED);
    assert(esp_openclaw_setup_submit(&f.session, f.generation, 1, 1, ESP_OPENCLAW_SETUP_GATEWAY_APPLY,
        dispatch, &f, &receipt) == ESP_OPENCLAW_SETUP_INVALID);
    assert(f.dispatched == 1);
    assert(!esp_openclaw_setup_complete(&f.session, f.token, ESP_OPENCLAW_SETUP_PENDING, 0));
    assert(esp_openclaw_setup_complete(&f.session, f.token, ESP_OPENCLAW_SETUP_SUCCEEDED, 7));
    assert(!esp_openclaw_setup_complete(&f.session, f.token, ESP_OPENCLAW_SETUP_FAILED, 0));
    assert(submit(&f, 1, &receipt) == ESP_OPENCLAW_SETUP_DUPLICATE);
    assert(receipt.outcome == ESP_OPENCLAW_SETUP_SUCCEEDED && receipt.detail == 7);
    assert(f.dispatched == 1);
    assert(submit(&f, 2, &receipt) == ESP_OPENCLAW_SETUP_ACCEPTED);
    assert(f.dispatched == 2);
    assert(esp_openclaw_setup_get_receipt(&f.session, f.generation, 1, 1, &receipt) == ESP_OPENCLAW_SETUP_OK);
    assert(receipt.outcome == ESP_OPENCLAW_SETUP_SUCCEEDED);
    assert(esp_openclaw_setup_get_receipt(&f.session, f.generation, 2, 1, &receipt) == ESP_OPENCLAW_SETUP_UNAUTHORIZED);
    assert(esp_openclaw_setup_get_receipt(&f.session, f.generation, 1, 3, &receipt) == ESP_OPENCLAW_SETUP_NOT_FOUND);
}

static void test_cancel_drains_before_reopen(void)
{
    fixture_t f;
    initialize(&f);
    confirm(&f);
    esp_openclaw_setup_receipt_t receipt;
    assert(submit(&f, 1, &receipt) == ESP_OPENCLAW_SETUP_ACCEPTED);
    esp_openclaw_setup_token_t old = receipt.token;
    assert(esp_openclaw_setup_detach(&f.session, f.generation, 1));
    esp_openclaw_setup_close(&f.session);
    esp_openclaw_setup_poll(&f.session);
    assert(f.canceled == 1);
    assert(esp_openclaw_setup_snapshot(&f.session).phase == ESP_OPENCLAW_SETUP_CLOSING);
    assert(!esp_openclaw_setup_authorize_operation(&f.session, old));
    assert(esp_openclaw_setup_open(&f.session, &f.generation) == ESP_OPENCLAW_SETUP_OCCUPIED);
    esp_openclaw_setup_token_t wrong = {old.generation + 1, old.operation_id};
    assert(!esp_openclaw_setup_complete(&f.session, wrong, ESP_OPENCLAW_SETUP_SUCCEEDED, 0));
    assert(esp_openclaw_setup_complete(&f.session, old, ESP_OPENCLAW_SETUP_SUCCEEDED, 0));
    assert(esp_openclaw_setup_snapshot(&f.session).receipt_count == 0);
    assert(esp_openclaw_setup_open(&f.session, &f.generation) == ESP_OPENCLAW_SETUP_OK);
    assert(f.generation != old.generation);
    assert(esp_openclaw_setup_input_released(&f.session, old.generation) == ESP_OPENCLAW_SETUP_STALE);
    confirm(&f);
    assert(!esp_openclaw_setup_detach(&f.session, old.generation, 1));
    assert(submit(&f, 1, &receipt) == ESP_OPENCLAW_SETUP_ACCEPTED);
    wrong = (esp_openclaw_setup_token_t){receipt.token.generation, receipt.token.operation_id + 1};
    assert(!esp_openclaw_setup_complete(&f.session, wrong, ESP_OPENCLAW_SETUP_SUCCEEDED, 0));
    assert(!esp_openclaw_setup_complete(&f.session, old, ESP_OPENCLAW_SETUP_SUCCEEDED, 0));
    assert(esp_openclaw_setup_snapshot(&f.session).phase == ESP_OPENCLAW_SETUP_BUSY);
    assert(esp_openclaw_setup_authorize_operation(&f.session, receipt.token));
}

static void test_live_authority_at_dispatch_and_after_wait(void)
{
    fixture_t f;
    initialize(&f);
    confirm(&f);
    esp_openclaw_setup_receipt_t receipt;
    f.revoke_after_check = true;
    assert(submit(&f, 1, &receipt) == ESP_OPENCLAW_SETUP_STALE);
    assert(f.dispatched == 0);
    assert(esp_openclaw_setup_snapshot(&f.session).phase == ESP_OPENCLAW_SETUP_CLOSED);

    initialize(&f);
    confirm(&f);
    assert(submit(&f, 1, &receipt) == ESP_OPENCLAW_SETUP_ACCEPTED);
    f.authenticated = false;
    assert(!esp_openclaw_setup_authorize_operation(&f.session, receipt.token));
    assert(f.canceled == 1 && f.applied == 0);
    assert(esp_openclaw_setup_complete(&f.session, receipt.token, ESP_OPENCLAW_SETUP_UNKNOWN, 0));
    assert(esp_openclaw_setup_snapshot(&f.session).phase == ESP_OPENCLAW_SETUP_CLOSED);

    initialize(&f);
    confirm(&f);
    assert(submit(&f, 1, &receipt) == ESP_OPENCLAW_SETUP_ACCEPTED);
    f.authenticated = false;
    assert(esp_openclaw_setup_complete(&f.session, receipt.token, ESP_OPENCLAW_SETUP_SUCCEEDED, 0));
    assert(esp_openclaw_setup_snapshot(&f.session).phase == ESP_OPENCLAW_SETUP_CLOSED);
    assert(f.canceled == 0);
}

static void test_timeout_and_synchronous_callbacks(void)
{
    fixture_t f;
    initialize(&f);
    f.now += ESP_OPENCLAW_SETUP_TIMEOUT_MS;
    assert(esp_openclaw_setup_attach(&f.session, f.generation, 1) == ESP_OPENCLAW_SETUP_STALE);
    initialize(&f);
    confirm(&f);
    esp_openclaw_setup_receipt_t receipt;
    f.complete_in_dispatch = true;
    assert(submit(&f, 1, &receipt) == ESP_OPENCLAW_SETUP_ACCEPTED);
    assert(esp_openclaw_setup_get_receipt(&f.session, f.generation, 1, 1, &receipt) == ESP_OPENCLAW_SETUP_OK);
    assert(receipt.outcome == ESP_OPENCLAW_SETUP_FAILED && receipt.detail == 17);
    assert(f.applied == 1 && esp_openclaw_setup_snapshot(&f.session).phase == ESP_OPENCLAW_SETUP_READY);
    f.complete_in_dispatch = false;
    assert(submit(&f, 2, &receipt) == ESP_OPENCLAW_SETUP_ACCEPTED);
    f.complete_in_cancel = true;
    f.now += ESP_OPENCLAW_SETUP_TIMEOUT_MS;
    esp_openclaw_setup_poll(&f.session);
    assert(f.canceled == 1 && esp_openclaw_setup_snapshot(&f.session).phase == ESP_OPENCLAW_SETUP_CLOSED);
    assert(esp_openclaw_setup_open(&f.session, &f.generation) == ESP_OPENCLAW_SETUP_OK);
}

static void test_capacity_never_evicts_receipts(void)
{
    fixture_t f;
    initialize(&f);
    confirm(&f);
    esp_openclaw_setup_receipt_t receipt;
    f.complete_in_dispatch = true;
    for (uint32_t id = 1; id <= ESP_OPENCLAW_SETUP_RECEIPT_LIMIT; ++id)
        assert(submit(&f, id, &receipt) == ESP_OPENCLAW_SETUP_ACCEPTED);
    assert(submit(&f, 33, &receipt) == ESP_OPENCLAW_SETUP_FULL);
    assert(submit(&f, 1, &receipt) == ESP_OPENCLAW_SETUP_DUPLICATE);
    assert(receipt.outcome == ESP_OPENCLAW_SETUP_FAILED);
    assert(f.dispatched == ESP_OPENCLAW_SETUP_RECEIPT_LIMIT);
    esp_openclaw_setup_close(&f.session);
    assert(esp_openclaw_setup_open(&f.session, &f.generation) == ESP_OPENCLAW_SETUP_OK);
    confirm(&f);
    assert(submit(&f, 33, &receipt) == ESP_OPENCLAW_SETUP_ACCEPTED);
}

int main(void)
{
    test_local_confirmation_and_peer_ownership();
    test_receipts_and_single_pending_operation();
    test_cancel_drains_before_reopen();
    test_live_authority_at_dispatch_and_after_wait();
    test_timeout_and_synchronous_callbacks();
    test_capacity_never_evicts_receipts();
    puts("setup session tests passed");
    return 0;
}
