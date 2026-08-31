/* Frozen aggregates from baseline 525f387a. Include the real public headers;
 * the comparison structs are intentionally independent of their definitions. */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "esp_openclaw_talk.h"
#include "esp_openclaw_node.h"

typedef struct {
    esp_openclaw_node_handle_t operator_node;
    const char *gateway_http_base_url;
    const char *session_key;
    const char *provider;
    const char *model;
    const char *voice;
    esp_openclaw_talk_setup_failed_cb_t setup_failed_cb;
    void *setup_failed_ctx;
    uint16_t silence_duration_ms;
} baseline_talk_config_t;

typedef struct {
    const char *display_name;
    const char *platform;
    const char *device_family;
    const char *client_id;
    const char *client_mode;
    const char *role;
    const char *model_identifier;
    const char *locale;
    const char *tls_common_name;
    const char *tls_cert_pem;
    size_t tls_cert_len;
    bool use_cert_bundle;
    bool skip_cert_common_name_check;
    esp_openclaw_node_event_cb_t event_cb;
    void *event_user_ctx;
    esp_openclaw_node_gateway_event_cb_t gateway_event_cb;
    void *gateway_event_user_ctx;
} baseline_node_config_t;

#define SAME_LAYOUT(type, baseline) \
    _Static_assert(sizeof(type) == sizeof(baseline), "aggregate size changed"); \
    _Static_assert(_Alignof(type) == _Alignof(baseline), "aggregate alignment changed")
#define TALK_OFFSET(field) _Static_assert(offsetof(esp_openclaw_talk_signaling_config_t, field) == \
    offsetof(baseline_talk_config_t, field), "Talk offset changed: " #field)
#define NODE_OFFSET(field) _Static_assert(offsetof(esp_openclaw_node_config_t, field) == \
    offsetof(baseline_node_config_t, field), "Node offset changed: " #field)
SAME_LAYOUT(esp_openclaw_talk_signaling_config_t, baseline_talk_config_t);
SAME_LAYOUT(esp_openclaw_node_config_t, baseline_node_config_t);
TALK_OFFSET(operator_node);
TALK_OFFSET(gateway_http_base_url);
TALK_OFFSET(session_key);
TALK_OFFSET(provider);
TALK_OFFSET(model);
TALK_OFFSET(voice);
TALK_OFFSET(setup_failed_cb);
TALK_OFFSET(setup_failed_ctx);
TALK_OFFSET(silence_duration_ms);
NODE_OFFSET(display_name);
NODE_OFFSET(platform);
NODE_OFFSET(device_family);
NODE_OFFSET(client_id);
NODE_OFFSET(client_mode);
NODE_OFFSET(role);
NODE_OFFSET(model_identifier);
NODE_OFFSET(locale);
NODE_OFFSET(tls_common_name);
NODE_OFFSET(tls_cert_pem);
NODE_OFFSET(tls_cert_len);
NODE_OFFSET(use_cert_bundle);
NODE_OFFSET(skip_cert_common_name_check);
NODE_OFFSET(event_cb);
NODE_OFFSET(event_user_ctx);
NODE_OFFSET(gateway_event_cb);
NODE_OFFSET(gateway_event_user_ctx);

static void failure(esp_openclaw_talk_setup_result_t result, void *ctx) { (void)result; (void)ctx; }
static void event(esp_openclaw_node_handle_t node, esp_openclaw_node_event_t type, const void *data, void *ctx)
{ (void)node; (void)type; (void)data; (void)ctx; }
static void gateway_event(esp_openclaw_node_handle_t node, const char *type, const char *payload, void *ctx)
{ (void)node; (void)type; (void)payload; (void)ctx; }
static int context;

int main(void)
{
    const esp_openclaw_talk_signaling_config_t talk_positional = {
        (esp_openclaw_node_handle_t)&context, "https://gateway.example", "main", "provider", "model", "voice",
        failure, &context, 400,
    };
    const esp_openclaw_talk_signaling_config_t talk_designated = {
        .operator_node = (esp_openclaw_node_handle_t)&context, .gateway_http_base_url = "https://gateway.example",
        .session_key = "main", .provider = "provider", .model = "model", .voice = "voice",
        .setup_failed_cb = failure, .setup_failed_ctx = &context, .silence_duration_ms = 400,
    };
    const esp_openclaw_node_config_t node_positional = {
        "display", "platform", "family", "id", "mode", "operator", "model", "en", "example", "certificate",
        11, true, false, event, &context, gateway_event, &context,
    };
    const esp_openclaw_node_config_t node_designated = {
        .display_name = "display", .platform = "platform", .device_family = "family", .client_id = "id",
        .client_mode = "mode", .role = "operator", .model_identifier = "model", .locale = "en",
        .tls_common_name = "example", .tls_cert_pem = "certificate", .tls_cert_len = 11,
        .use_cert_bundle = true, .skip_cert_common_name_check = false, .event_cb = event,
        .event_user_ctx = &context, .gateway_event_cb = gateway_event, .gateway_event_user_ctx = &context,
    };
#define TALK_VALUE(field) assert(talk_positional.field == talk_designated.field)
#define NODE_VALUE(field) assert(node_positional.field == node_designated.field)
#define TALK_STRING(field) assert(strcmp(talk_positional.field, talk_designated.field) == 0)
#define NODE_STRING(field) assert(strcmp(node_positional.field, node_designated.field) == 0)
    TALK_VALUE(operator_node); TALK_STRING(gateway_http_base_url); TALK_STRING(session_key);
    TALK_STRING(provider); TALK_STRING(model); TALK_STRING(voice); TALK_VALUE(setup_failed_cb);
    TALK_VALUE(setup_failed_ctx); TALK_VALUE(silence_duration_ms);
    NODE_STRING(display_name); NODE_STRING(platform); NODE_STRING(device_family); NODE_STRING(client_id);
    NODE_STRING(client_mode); NODE_STRING(role); NODE_STRING(model_identifier); NODE_STRING(locale);
    NODE_STRING(tls_common_name); NODE_STRING(tls_cert_pem); NODE_VALUE(tls_cert_len);
    NODE_VALUE(use_cert_bundle); NODE_VALUE(skip_cert_common_name_check); NODE_VALUE(event_cb);
    NODE_VALUE(event_user_ctx); NODE_VALUE(gateway_event_cb); NODE_VALUE(gateway_event_user_ctx);
    printf("legacy positional/designated ABI: Talk size=%zu align=%zu; Node size=%zu align=%zu; all offsets PASS\n",
        sizeof(talk_positional), _Alignof(esp_openclaw_talk_signaling_config_t),
        sizeof(node_positional), _Alignof(esp_openclaw_node_config_t));
    return 0;
}
