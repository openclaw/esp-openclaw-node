#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    assert(size > 0);
    assert(fseek(file, 0, SEEK_SET) == 0);
    char *buf = malloc((size_t)size + 1U);
    assert(buf != NULL);
    assert(fread(buf, 1, (size_t)size, file) == (size_t)size);
    buf[size] = '\0';
    fclose(file);
    return buf;
}

static const char *function_end(const char *fn)
{
    const char *next = strstr(fn + 1, "\nstatic ");
    return next != NULL ? next : fn + strlen(fn);
}

static int exchange_sdp_disables_auto_redirect(const char *src)
{
    const char *fn = strstr(src, "static int exchange_sdp(");
    if (fn == NULL) {
        return -1;
    }
    const char *end = function_end(fn);
    const char *cfg = strstr(fn, "esp_http_client_config_t config");
    if (cfg == NULL || cfg >= end) {
        return -1;
    }
    const char *block_end = strstr(cfg, "};");
    if (block_end == NULL || block_end >= end) {
        return -1;
    }
    size_t n = (size_t)(block_end - cfg);
    char *block = malloc(n + 1U);
    assert(block != NULL);
    memcpy(block, cfg, n);
    block[n] = '\0';
    int result = 0;
    if (strstr(block, ".disable_auto_redirect = true") != NULL) {
        result = 1;
    }
    free(block);
    return result;
}

static bool exchange_sdp_posts_bearer(const char *src)
{
    const char *fn = strstr(src, "static int exchange_sdp(");
    if (fn == NULL) {
        return false;
    }
    const char *end = function_end(fn);
    const char *auth = strstr(fn, "Authorization");
    const char *bearer = strstr(fn, "Bearer ");
    return auth != NULL && auth < end && bearer != NULL && bearer < end;
}

typedef struct {
    char host[64];
    char authorization[64];
} hop_request_t;

static int post_with_optional_follow(
    bool disable_auto_redirect,
    const hop_request_t *offer,
    const char *location_host,
    hop_request_t *hop)
{
    memset(hop, 0, sizeof(*hop));
    if (disable_auto_redirect) {
        return 302;
    }
    snprintf(hop->host, sizeof(hop->host), "%s", location_host);
    snprintf(hop->authorization, sizeof(hop->authorization), "%s", offer->authorization);
    return 200;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1
        ? argv[1]
        : "components/esp-openclaw-talk/src/esp_openclaw_talk.c";
    char *src = read_file(path);
    assert(exchange_sdp_posts_bearer(src));
    assert(exchange_sdp_disables_auto_redirect(src) == 1);

    hop_request_t offer = {
        .host = "offer.example",
        .authorization = "Bearer broker-token",
    };
    hop_request_t hop;
    assert(post_with_optional_follow(false, &offer, "evil.example", &hop) == 200);
    assert(strcmp(hop.host, "evil.example") == 0);
    assert(strcmp(hop.authorization, "Bearer broker-token") == 0);
    assert(post_with_optional_follow(true, &offer, "evil.example", &hop) == 302);
    assert(hop.host[0] == '\0');
    assert(hop.authorization[0] == '\0');

    free(src);
    puts("talk SDP HTTP redirect tests passed");
    return 0;
}
