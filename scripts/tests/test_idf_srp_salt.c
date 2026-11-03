#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "srp_sdk.h"
#include "esp_srp.h"

static size_t allocations;
static size_t random_calls;
static size_t zero_prefix;

void *srp_test_malloc(size_t size)
{
    void *result = malloc(size);
    if (result) ++allocations;
    return result;
}

void *srp_test_calloc(size_t count, size_t size)
{
    void *result = calloc(count, size);
    if (result) ++allocations;
    return result;
}

void srp_test_free(void *pointer)
{
    if (pointer) {
        assert(allocations > 0);
        --allocations;
        free(pointer);
    }
}

void esp_fill_random(void *buffer, size_t length)
{
    assert(length == 16);
    ++random_calls;
    memset(buffer, 0x5a, length);
    memset(buffer, 0, zero_prefix);
}

int main(void)
{
    /* Use the public helper, its real MPI exporter and real SHA-512/modexp. */
    for (zero_prefix = 0; zero_prefix <= 16; ++zero_prefix) {
        char *salt = NULL;
        char *verifier = NULL;
        int verifier_length = 0;
        esp_err_t result = esp_srp_gen_salt_verifier(
            "openclaw", 8, "synthetic-test-password", 23,
            &salt, 16, &verifier, &verifier_length);
        assert(random_calls == zero_prefix + 1);
        if (zero_prefix) {
            assert(result == ESP_FAIL);
            assert(salt == NULL && verifier == NULL && verifier_length == 0);
        } else {
            assert(result == ESP_OK && salt && verifier);
            assert(verifier_length > 0 && verifier_length <= 384);
            for (size_t i = 0; i < 16; ++i) assert((unsigned char)salt[i] == 0x5a);
            srp_test_free(salt);
            srp_test_free(verifier);
        }
        assert(allocations == 0);
    }
    puts("actual IDF SRP/MPI: full-width success, all short salts rejected, cleanup balanced");
    return 0;
}
