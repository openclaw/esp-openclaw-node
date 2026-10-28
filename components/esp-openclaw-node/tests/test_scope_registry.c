#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

typedef int esp_err_t;
enum { ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE };
enum { ESP_OPENCLAW_NODE_INTERNAL_IDLE, CONNECTING, READY, CLOSING, DESTROYING };
enum { ESP_OPENCLAW_NODE_PENDING_CONTROL_REQUEST_NONE, CONNECT_REQUEST, DISCONNECT_REQUEST };
typedef struct {
    int state, pending_control;
    char *scopes[4];
    size_t scope_count;
} node_fixture_t;
typedef node_fixture_t *esp_openclaw_node_handle_t;

static bool locked;
static int state_on_lock = -1;
static void esp_openclaw_node_lock_state(esp_openclaw_node_handle_t node)
{
    assert(!locked);
    locked = true;
    if (state_on_lock >= 0) node->state = state_on_lock;
}
static void esp_openclaw_node_unlock_state(esp_openclaw_node_handle_t node)
{
    (void)node;
    assert(locked);
    locked = false;
}

#include "production.inc"

static void expect_scopes(esp_openclaw_node_handle_t node, const char *expected)
{
    assert(!locked);
    cJSON *hello = cJSON_CreateObject();
    esp_openclaw_node_add_registered_string_array(hello, "scopes", node->scopes, node->scope_count);
    char *json = cJSON_PrintUnformatted(hello);
    assert(json != NULL && strcmp(json, expected) == 0);
    free(json);
    cJSON_Delete(hello);
}

int main(void)
{
    node_fixture_t storage = {0};
    /* Use the real registry's fields; only the SDK mutex and unrelated node
     * fields are replaced at this host boundary. */
    esp_openclaw_node_handle_t node = &storage;
    node->scopes[0] = strdup("operator.read");
    node->scopes[1] = strdup("operator.write");
    node->scopes[2] = strdup("operator.talk");
    node->scope_count = 3;
    assert(esp_openclaw_node_unregister_scope_internal(NULL, "operator.write") == ESP_ERR_INVALID_ARG);
    assert(esp_openclaw_node_unregister_scope_internal(node, NULL) == ESP_ERR_INVALID_ARG);
    assert(esp_openclaw_node_unregister_scope_internal(node, "") == ESP_ERR_INVALID_ARG);
    for (int state = CONNECTING; state <= DESTROYING; ++state) {
        node->state = state;
        assert(esp_openclaw_node_unregister_scope_internal(node, "operator.write") == ESP_ERR_INVALID_STATE);
    }
    node->state = ESP_OPENCLAW_NODE_INTERNAL_IDLE;
    for (int pending = CONNECT_REQUEST; pending <= DISCONNECT_REQUEST; ++pending) {
        node->pending_control = pending;
        assert(esp_openclaw_node_unregister_scope_internal(node, "operator.write") == ESP_ERR_INVALID_STATE);
    }
    node->pending_control = ESP_OPENCLAW_NODE_PENDING_CONTROL_REQUEST_NONE;
    state_on_lock = READY;
    assert(esp_openclaw_node_unregister_scope_internal(node, "operator.write") == ESP_ERR_INVALID_STATE);
    state_on_lock = -1;
    expect_scopes(node, "{\"scopes\":[\"operator.read\",\"operator.write\",\"operator.talk\"]}");
    node->state = ESP_OPENCLAW_NODE_INTERNAL_IDLE;
    assert(esp_openclaw_node_unregister_scope_internal(node, "operator.write") == ESP_OK);
    assert(esp_openclaw_node_unregister_scope_internal(node, "operator.write") == ESP_OK);
    expect_scopes(node, "{\"scopes\":[\"operator.read\",\"operator.talk\"]}");
    assert(esp_openclaw_node_unregister_scope_internal(node, "operator.read") == ESP_OK);
    expect_scopes(node, "{\"scopes\":[\"operator.talk\"]}");
    assert(esp_openclaw_node_unregister_scope_internal(node, "operator.talk") == ESP_OK);
    expect_scopes(node, "{\"scopes\":[]}");
    assert(node->scopes[0] == NULL);
    puts("PASS scope removal: idle authority, pending controls, idempotence and handshake scopes");
    return 0;
}
