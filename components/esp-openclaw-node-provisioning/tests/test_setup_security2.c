#include "esp_openclaw_setup_security2.h"
#include "esp_openclaw_setup_session.h"
#include "protocomm_security2.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* This fixture implements only the public SDK call boundary. Its bytes are not
 * cryptography: real SRP/AES-GCM exchange belongs to device/phone qualification. */
static unsigned starts, closes, requests, encryptions, decryptions, cleanups, wiped, initializations;
static unsigned quiet_tags;
static bool fail_start, fail_crypto, revoke_crypto, fail_quiet;
static uint8_t *vendor_buffer;
static size_t vendor_length;
static const void *expected_params, *expected_context;
static int sdk_context;

void esp_log_level_set(const char *tag, int level)
{
    const char *tags[] = {"security2", "srp6a", "protocomm", "protocomm_nimble", "protocomm_ble"};
    assert(level == 0);
    bool found = false;
    for (unsigned i = 0; i < 5; ++i) if (strcmp(tag, tags[i]) == 0) { quiet_tags |= 1U << i; found = true; }
    assert(found);
}
int esp_log_level_get(const char *tag) { (void)tag; return fail_quiet ? 3 : 0; }

/* The runner redirects only the production object's free(), observing rejected
 * SDK allocations at release without a test API or private-struct inspection. */
void security_test_free(void *pointer)
{
    if (pointer == vendor_buffer && pointer) {
        for (size_t i = 0; i < vendor_length; ++i) assert(vendor_buffer[i] == 0);
        vendor_buffer = NULL;
        ++wiped;
    }
    free(pointer);
}

static esp_err_t sdk_init(protocomm_security_handle_t *handle)
{ assert(quiet_tags == 31); ++initializations; *handle = &sdk_context; return ESP_OK; }
static esp_err_t sdk_cleanup(protocomm_security_handle_t handle)
{ assert(handle == &sdk_context && esp_openclaw_setup_security2_peer() == 0); ++cleanups; return ESP_OK; }
static esp_err_t sdk_open(protocomm_security_handle_t handle, uint32_t id)
{ assert(handle == &sdk_context && id != 0); ++starts; return fail_start ? ESP_FAIL : ESP_OK; }
static esp_err_t sdk_close(protocomm_security_handle_t handle, uint32_t id)
{ assert(handle == &sdk_context && id != 0 && esp_openclaw_setup_security2_peer() == 0); ++closes; return ESP_OK; }
static esp_err_t sdk_request(protocomm_security_handle_t handle, const void *params, uint32_t id,
    const uint8_t *input, ssize_t size, uint8_t **output, ssize_t *length, void *context)
{
    assert(handle == &sdk_context && id == 7 && params == expected_params && context == expected_context);
    assert(size == 3 && memcmp(input, "srp", 3) == 0);
    ++requests; *output = NULL; *length = 0;
    return ESP_OK;
}
static esp_err_t sdk_crypt(protocomm_security_handle_t handle, uint32_t id, const uint8_t *input,
    ssize_t size, uint8_t **output, ssize_t *length, bool encrypting)
{
    assert(handle == &sdk_context && id != 0 && input && !vendor_buffer);
    if (encrypting) ++encryptions; else ++decryptions;
    vendor_length = (size_t)(size + (encrypting ? 16 : -16));
    vendor_buffer = malloc(vendor_length);
    assert(vendor_buffer);
    memset(vendor_buffer, 0xa5, vendor_length);
    *output = vendor_buffer; *length = (ssize_t)vendor_length;
    if (revoke_crypto) esp_openclaw_setup_security2_revoke();
    return fail_crypto ? ESP_FAIL : ESP_OK;
}
static esp_err_t sdk_encrypt(protocomm_security_handle_t h, uint32_t id, const uint8_t *b,
    ssize_t n, uint8_t **o, ssize_t *s) { return sdk_crypt(h, id, b, n, o, s, true); }
static esp_err_t sdk_decrypt(protocomm_security_handle_t h, uint32_t id, const uint8_t *b,
    ssize_t n, uint8_t **o, ssize_t *s) { return sdk_crypt(h, id, b, n, o, s, false); }
const protocomm_security_t protocomm_security2 = {.ver = 2, .patch_ver = 1,
    .init = sdk_init, .cleanup = sdk_cleanup, .new_transport_session = sdk_open,
    .close_transport_session = sdk_close, .security_req_handler = sdk_request,
    .encrypt = sdk_encrypt, .decrypt = sdk_decrypt};

static void release_result(uint8_t *result)
{ assert(result == vendor_buffer); vendor_buffer = NULL; free(result); }
static uint64_t now(void *context) { (void)context; return 1; }
static bool live(uint64_t peer, void *context)
{ (void)context; return esp_openclaw_setup_security2_peer_is_current(peer); }
static void cancel(esp_openclaw_setup_token_t token, void *context)
{ (void)token; (void)context; assert(false); }

int main(int argc, char **argv)
{
    const protocomm_security_t *guard = esp_openclaw_setup_security2();
    if (argc == 2 && strcmp(argv[1], "unsupported") == 0) {
        assert(!guard && esp_openclaw_setup_security2_peer() == 0);
        assert(!esp_openclaw_setup_security2_peer_is_current(1));
        assert(!esp_openclaw_setup_security2_quiet_logs()); esp_openclaw_setup_security2_revoke();
        assert(!quiet_tags && !starts && !requests && !decryptions);
        puts("Security2 unavailable profile: no SDK calls or peer authority");
        return 0;
    }
    assert(guard);
    protocomm_security_handle_t handle, second;
    fail_quiet = true;
    assert(guard->init(&handle) == ESP_ERR_INVALID_STATE && !initializations);
    fail_quiet = false;
    assert(guard->init(&handle) == ESP_OK && guard->init(&second) == ESP_ERR_INVALID_STATE);
    assert(guard->new_transport_session(handle, 7) == ESP_OK && starts == 1);
    assert(!esp_openclaw_setup_security2_peer());
    uint8_t input[481] = {0}, *output = NULL;
    ssize_t length = -1;
    int params, context;
    expected_params = &params; expected_context = &context;
    assert(guard->security_req_handler(handle, &params, 7, (const uint8_t *)"srp", 3,
        &output, &length, &context) == ESP_OK && requests == 1);
    assert(!esp_openclaw_setup_security2_peer()); /* Handshake/CONNECTED is not authority. */
    assert(guard->encrypt(handle, 7, input, 1, &output, &length) == ESP_ERR_INVALID_STATE && !encryptions);
    assert(guard->decrypt(handle, 7, input, 480, &output, &length) == ESP_OK && length == 464);
    release_result(output);
    uint64_t peer = esp_openclaw_setup_security2_peer();
    assert(peer && live(peer, NULL));
    assert(guard->new_transport_session(handle, 8) == ESP_ERR_INVALID_STATE && starts == 1);
    assert(guard->close_transport_session(handle, 8) == ESP_ERR_INVALID_STATE && !closes && live(peer, NULL));
    assert(guard->decrypt(handle, 8, input, 17, &output, &length) == ESP_ERR_INVALID_STATE && decryptions == 1);
    assert(live(peer, NULL)); /* A different transport cannot displace the first. */
    assert(guard->encrypt(handle, 7, input, 464, &output, &length) == ESP_OK && length == 480);
    release_result(output);
    assert(guard->encrypt(handle, 7, input, 465, &output, &length) == ESP_ERR_INVALID_ARG);
    assert(guard->decrypt(handle, 7, input, 481, &output, &length) == ESP_ERR_INVALID_ARG);
    assert(guard->decrypt(handle, 7, input, 16, &output, &length) == ESP_ERR_INVALID_ARG);
    assert(encryptions == 1 && decryptions == 1 && live(peer, NULL));

    esp_openclaw_setup_session_t session;
    esp_openclaw_setup_callbacks_t callbacks = {.now_ms = now, .peer_is_current_authenticated = live, .cancel = cancel};
    uint64_t generation;
    assert(esp_openclaw_setup_init(&session, &callbacks));
    assert(esp_openclaw_setup_open(&session, &generation) == ESP_OPENCLAW_SETUP_OK);
    assert(esp_openclaw_setup_attach(&session, generation, peer) == ESP_OPENCLAW_SETUP_OK);
    assert(esp_openclaw_setup_input_released(&session, generation) == ESP_OPENCLAW_SETUP_OK);
    assert(esp_openclaw_setup_confirm(&session, generation) == ESP_OPENCLAW_SETUP_OK);
    /* Revocation while the vendor callback runs must withhold and wipe output. */
    revoke_crypto = true; output = NULL; length = -1;
    assert(guard->encrypt(handle, 7, input, 1, &output, &length) == ESP_ERR_INVALID_STATE);
    assert(!output && length == -1 && wiped == 1 && !live(peer, NULL));
    assert(esp_openclaw_setup_authorize_peer(&session, generation, peer) != ESP_OPENCLAW_SETUP_OK);
    assert(esp_openclaw_setup_snapshot(&session).phase == ESP_OPENCLAW_SETUP_CLOSED);
    revoke_crypto = false;
    assert(guard->close_transport_session(handle, 7) == ESP_OK && closes == 1);
    assert(guard->new_transport_session(handle, 7) == ESP_OK && starts == 2);
    assert(guard->decrypt(handle, 7, input, 17, &output, &length) == ESP_OK);
    release_result(output);
    uint64_t next_peer = esp_openclaw_setup_security2_peer();
    assert(next_peer > peer && !live(peer, NULL));
    assert(guard->security_req_handler(handle, &params, 7, input, 1, &output, &length, &context) == ESP_ERR_INVALID_STATE);
    assert(requests == 1 && !live(next_peer, NULL)); /* Rehandshake revokes before delegation. */
    assert(guard->close_transport_session(handle, 7) == ESP_OK);
    assert(guard->new_transport_session(handle, 7) == ESP_OK);
    assert(guard->decrypt(handle, 7, input, 17, &output, &length) == ESP_OK);
    release_result(output);
    next_peer = esp_openclaw_setup_security2_peer();
    assert(guard->new_transport_session(handle, 7) == ESP_ERR_INVALID_STATE && starts == 3);
    assert(!live(next_peer, NULL)); /* A reused same-ID new-session is also rehandshake. */
    assert(guard->cleanup(handle) == ESP_OK && cleanups == 1);

    assert(guard->init(&handle) == ESP_OK && guard->new_transport_session(handle, 7) == ESP_OK);
    assert(guard->decrypt(handle, 7, input, 17, &output, &length) == ESP_OK);
    release_result(output);
    assert(esp_openclaw_setup_security2_peer() > next_peer); /* Start/stop never reuses a peer. */
    peer = esp_openclaw_setup_security2_peer();
    assert(guard->close_transport_session(handle, 7) == ESP_OK && !live(peer, NULL));
    assert(guard->new_transport_session(handle, 7) == ESP_OK);
    revoke_crypto = true; output = NULL; length = -1;
    assert(guard->decrypt(handle, 7, input, 17, &output, &length) == ESP_ERR_INVALID_STATE);
    assert(!output && length == -1 && wiped == 2 && !esp_openclaw_setup_security2_peer());
    revoke_crypto = false;
    assert(guard->close_transport_session(handle, 7) == ESP_OK);
    assert(guard->new_transport_session(handle, 7) == ESP_OK);
    assert(guard->decrypt(handle, 7, input, 17, &output, &length) == ESP_OK);
    release_result(output);
    fail_crypto = true; output = NULL; length = -1;
    assert(guard->decrypt(handle, 7, input, 17, &output, &length) == ESP_FAIL);
    assert(!output && length == -1 && wiped == 3 && !esp_openclaw_setup_security2_peer());
    assert(guard->close_transport_session(handle, 7) == ESP_OK);
    fail_crypto = false; fail_start = true;
    assert(guard->new_transport_session(handle, 9) == ESP_FAIL);
    fail_start = false;
    assert(guard->new_transport_session(handle, 9) == ESP_OK);
    assert(guard->decrypt(handle, 9, input, 17, &output, &length) == ESP_OK);
    release_result(output);
    peer = esp_openclaw_setup_security2_peer();
    assert(guard->cleanup(handle) == ESP_OK && !live(peer, NULL));
    puts("Security2 guard: SDK delegation, peer exclusivity/revocation, live session authority, bounds and wiping passed");
    return 0;
}
