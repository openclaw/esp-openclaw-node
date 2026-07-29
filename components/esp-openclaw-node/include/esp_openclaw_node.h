/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Limits */

/** @brief Maximum number of capabilities a node can advertise. */
#define ESP_OPENCLAW_NODE_MAX_CAPABILITIES CONFIG_ESP_OPENCLAW_NODE_MAX_CAPABILITIES
/** @brief Maximum number of scopes a client can advertise. */
#define ESP_OPENCLAW_NODE_MAX_SCOPES CONFIG_ESP_OPENCLAW_NODE_MAX_SCOPES
/** @brief Maximum number of commands a node can register. */
#define ESP_OPENCLAW_NODE_MAX_COMMANDS CONFIG_ESP_OPENCLAW_NODE_MAX_COMMANDS

/* Core Types */

/** @brief Opaque handle for an OpenClaw Node instance. */
typedef struct esp_openclaw_node *esp_openclaw_node_handle_t;

/** @brief Generic Gateway event delivered after the authenticated session is ready. */
typedef void (*esp_openclaw_node_gateway_event_cb_t)(
    esp_openclaw_node_handle_t node,
    const char *event,
    const char *payload_json,
    void *user_ctx);

/** @brief Result of one asynchronous Gateway RPC request. */
typedef struct {
    bool ok;
    const char *payload_json;
    const char *error_code;
    const char *error_message;
} esp_openclaw_node_gateway_result_t;

/** @brief Completion callback for @ref esp_openclaw_node_gateway_request. */
typedef void (*esp_openclaw_node_gateway_request_cb_t)(
    esp_openclaw_node_handle_t node,
    const esp_openclaw_node_gateway_result_t *result,
    void *user_ctx);

/* Event Types */

/** @brief Terminal and maintenance events emitted by the component. */
typedef enum {
    ESP_OPENCLAW_NODE_EVENT_CONNECTED = 0,  /**< Handshake completed and the session is ready. */
    ESP_OPENCLAW_NODE_EVENT_CONNECT_FAILED, /**< A connect attempt ended without reaching ready state. */
    ESP_OPENCLAW_NODE_EVENT_DISCONNECTED,   /**< An established session disconnected or was closed locally. */
} esp_openclaw_node_event_t;

/** @brief Failure reasons surfaced on @ref ESP_OPENCLAW_NODE_EVENT_CONNECT_FAILED. */
typedef enum {
    ESP_OPENCLAW_NODE_CONNECT_FAILURE_TRANSPORT_START_FAILED = 0,  /**< Transport startup or local connect setup failed. */
    ESP_OPENCLAW_NODE_CONNECT_FAILURE_CONNECTION_LOST,             /**< The transport dropped before connect completed. */
    ESP_OPENCLAW_NODE_CONNECT_FAILURE_AUTH_REJECTED,               /**< The gateway rejected the auth material or signature. */
    ESP_OPENCLAW_NODE_CONNECT_FAILURE_SESSION_FINALIZATION_FAILED, /**< `hello-ok` handling failed after initial auth acceptance. */
} esp_openclaw_node_connect_failure_reason_t;

/** @brief Payload for @ref ESP_OPENCLAW_NODE_EVENT_CONNECT_FAILED. */
typedef struct {
    esp_openclaw_node_connect_failure_reason_t reason; /**< High-level failure category. */
    esp_err_t local_err;                               /**< Error code associated with the failure, or ESP_OK if none applies. */
    const char *gateway_detail_code;                   /**< Optional gateway detail code such as `AUTH_*`. */
} esp_openclaw_node_connect_failed_event_t;

/** @brief Disconnect reasons surfaced on @ref ESP_OPENCLAW_NODE_EVENT_DISCONNECTED. */
typedef enum {
    ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED = 0,   /**< Disconnect was requested locally. */
    ESP_OPENCLAW_NODE_DISCONNECTED_REASON_CONNECTION_LOST, /**< Disconnect followed a transport loss or remote close. */
} esp_openclaw_node_disconnected_reason_t;

/** @brief Payload for @ref ESP_OPENCLAW_NODE_EVENT_DISCONNECTED. */
typedef struct {
    esp_openclaw_node_disconnected_reason_t reason; /**< High-level disconnect category. */
    esp_err_t local_err;                            /**< Error code associated with the disconnect, or ESP_OK if none applies. */
} esp_openclaw_node_disconnected_event_t;

/**
 * @brief Node event callback.
 *
 * Event callbacks run on the component's internal node task. Callback code
 * must stay short and non-blocking. It may call the async
 * `esp_openclaw_node_request_*()` APIs. It must not call
 * @ref esp_openclaw_node_destroy.
 *
 * For each accepted connect request, wait for exactly one terminal outcome:
 * - @ref ESP_OPENCLAW_NODE_EVENT_CONNECTED
 * - @ref ESP_OPENCLAW_NODE_EVENT_CONNECT_FAILED
 * - @ref ESP_OPENCLAW_NODE_EVENT_DISCONNECTED after an accepted disconnect of an
 *   active session
 *
 * `event_data` is:
 * - `NULL` for @ref ESP_OPENCLAW_NODE_EVENT_CONNECTED
 * - @ref esp_openclaw_node_connect_failed_event_t for
 *   @ref ESP_OPENCLAW_NODE_EVENT_CONNECT_FAILED
 * - @ref esp_openclaw_node_disconnected_event_t for
 *   @ref ESP_OPENCLAW_NODE_EVENT_DISCONNECTED
 *
 * The payload pointer is valid only for the duration of the callback.
 */
typedef void (*esp_openclaw_node_event_cb_t)(
    esp_openclaw_node_handle_t node,
    esp_openclaw_node_event_t event,
    const void *event_data,
    void *user_ctx);

/* Configuration */

/**
 * @brief Node configuration.
 *
 * Before creating or connecting the node, the application must initialize the
 * ESP-IDF runtime pieces that networking depends on:
 * - `nvs_flash_init()`
 * - `esp_netif_init()`
 * - `esp_event_loop_create_default()`
 *
 * The application owns network setup. This component does not bring up Wi-Fi,
 * Ethernet, PPP, or any other network path; it opens the OpenClaw WebSocket
 * session once a route to the configured gateway exists.
 */
typedef struct {
    const char *display_name;      /**< Human-readable name advertised to the gateway. */
    const char *platform;          /**< Lower-level platform string included in auth and identity metadata. */
    const char *device_family;     /**< Higher-level device family string included in auth metadata. */
    const char *client_id;         /**< Protocol client identifier sent during connect. */
    const char *client_mode;       /**< Protocol client mode, typically `node` for these examples. */
    const char *role;              /**< Protocol role string expected by the gateway for this client. */
    const char *model_identifier;  /**< Optional model string such as `esp32c6` or `esp32s3`. */
    const char *locale;            /**< Optional locale metadata advertised to the gateway. */
    const char *tls_common_name;   /**< Optional TLS common-name override for certificate checks. */
    const char *tls_cert_pem;      /**< Optional PEM trust anchor for `wss://` connections. */
    size_t tls_cert_len;           /**< Length of @p tls_cert_pem in bytes, or `0` for NUL-terminated PEM. */
    bool use_cert_bundle;          /**< Use the ESP-IDF certificate bundle for server validation. */
    bool skip_cert_common_name_check; /**< Skip common-name validation for TLS server certificates. */
    esp_openclaw_node_event_cb_t event_cb; /**< Optional event callback invoked on the component's internal node task. */
    void *event_user_ctx;          /**< Opaque caller context passed back to @p event_cb. */
    esp_openclaw_node_gateway_event_cb_t gateway_event_cb; /**< Optional post-connect Gateway event callback. */
    void *gateway_event_user_ctx;  /**< Opaque context passed to @p gateway_event_cb. */
} esp_openclaw_node_config_t;

/* Command Types */

/** @brief Structured command error returned to the gateway. */
typedef struct {
    const char *code;    /**< Stable machine-readable error code returned to the gateway. */
    const char *message; /**< Human-readable error message returned to the gateway. */
} esp_openclaw_node_error_t;

/**
 * @brief Handler for a single advertised OpenClaw command.
 *
 * @param node Node instance handling the request.
 * @param context Optional user context from the command registration.
 * @param params_json UTF-8 JSON parameters from the gateway request. When the
 *        request omits `paramsJSON`, the component passes `"{}"`.
 * @param params_len Length of @p params_json in bytes, excluding the trailing
 *        `NUL`.
 * @param[out] out_payload_json Output location for the success payload JSON.
 *             The component initializes `*out_payload_json` to `NULL` before
 *             calling the handler. On success, leave `*out_payload_json` as
 *             `NULL` to send no payload, or assign a `malloc()`-compatible
 *             UTF-8 JSON string. The component takes ownership of any
 *             non-`NULL` buffer and frees it after sending `payloadJSON`.
 * @param[out] out_error Structured command error when the handler fails.
 *
 * @return
 *      - `ESP_OK` on success
 *      - another ESP-IDF error code on failure
 */
typedef esp_err_t (*esp_openclaw_node_command_handler_t)(
    esp_openclaw_node_handle_t node,
    void *context,
    const char *params_json,
    size_t params_len,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error);

/** @brief Command registration entry passed to esp_openclaw_node_register_command(). */
typedef struct {
    const char *name;                            /**< Command name advertised to the gateway, for example `wifi.status`. */
    esp_openclaw_node_command_handler_t handler; /**< Command callback invoked for matching requests. */
    void *context;                               /**< Opaque caller context passed to @p handler. */
} esp_openclaw_node_command_t;

/* Connect Input Types */

/** @brief Caller-chosen source for one connect attempt. */
typedef enum {
    ESP_OPENCLAW_NODE_CONNECT_SOURCE_SAVED_SESSION = 0, /**< Reconnect with the persisted `{ gateway_uri, device_token }` session. */
    ESP_OPENCLAW_NODE_CONNECT_SOURCE_SETUP_CODE,        /**< Decode and use a setup code that contains the gateway URI and exactly one auth secret. */
    ESP_OPENCLAW_NODE_CONNECT_SOURCE_DEVICE_TOKEN,      /**< Connect with an explicit role-scoped device token. */
    ESP_OPENCLAW_NODE_CONNECT_SOURCE_GATEWAY_TOKEN,     /**< Connect with an explicit shared gateway token. */
    ESP_OPENCLAW_NODE_CONNECT_SOURCE_GATEWAY_PASSWORD,  /**< Connect with an explicit shared gateway password. */
    ESP_OPENCLAW_NODE_CONNECT_SOURCE_NO_AUTH,           /**< Connect to a gateway that intentionally allows unauthenticated node access. */
} esp_openclaw_node_connect_source_t;

/**
 * @brief Connect request submitted to @ref esp_openclaw_node_request_connect().
 *
 * Field requirements by source:
 * - `SAVED_SESSION`:    `gateway_uri = NULL`,                 `value = NULL`
 * - `SETUP_CODE`:       `gateway_uri = NULL`,                 `value = <setup code>`
 * - `DEVICE_TOKEN`:     `gateway_uri = <ws://...|wss://...>`, `value = <device token>`
 * - `GATEWAY_TOKEN`:    `gateway_uri = <ws://...|wss://...>`, `value = <token>`
 * - `GATEWAY_PASSWORD`: `gateway_uri = <ws://...|wss://...>`, `value = <password>`
 * - `NO_AUTH`:          `gateway_uri = <ws://...|wss://...>`, `value = NULL`
 */
typedef struct {
    esp_openclaw_node_connect_source_t source; /**< Caller-chosen source for this one connect attempt. */
    const char *gateway_uri;                   /**< Explicit `ws://` or `wss://` URI when required by @p source. */
    const char *value;                         /**< Setup code, token, or password when required by @p source. */
} esp_openclaw_node_connect_request_t;

/* Lifecycle APIs */

/**
 * @brief Populate a config struct with the component's built-in defaults.
 *
 * @param[out] config Configuration struct to initialize.
 */
void esp_openclaw_node_config_init_default(esp_openclaw_node_config_t *config);

/**
 * @brief Create a node instance from the supplied configuration.
 *
 * This also creates the component queues and task used to serialize transport
 * and session state transitions.
 *
 * @param[in] config Configuration to copy into the node instance.
 * @param[out] out_node Created node handle.
 *
 * @return
 *      - `ESP_OK` on success
 *      - `ESP_ERR_INVALID_ARG` if `config` or `out_node` is `NULL`
 *      - `ESP_ERR_NO_MEM` if allocation of the node, queues, task, mutex, or
 *        copied config strings fails
 *      - another initialization error if identity setup or saved-session
 *        loading fails
 */
esp_err_t esp_openclaw_node_create(
    const esp_openclaw_node_config_t *config,
    esp_openclaw_node_handle_t *out_node);

/**
 * @brief Destroy a node instance and release all owned resources.
 *
 * This API is synchronous. It rejects self-destroy from the component task or
 * callback context.
 *
 * @param[in] node Node handle to destroy.
 *
 * @return
 *      - `ESP_OK` on success
 *      - `ESP_ERR_INVALID_ARG` if `node` is `NULL`
 *      - `ESP_ERR_INVALID_STATE` if destroy has already begun or the current
 *        task is the component task
 */
esp_err_t esp_openclaw_node_destroy(esp_openclaw_node_handle_t node);

/* Registration APIs */

/**
 * @brief Advertise a capability string such as `device` or `wifi`.
 *
 * @param[in] node Node handle.
 * @param[in] capability Capability name to register.
 *
 * @return
 *      - `ESP_OK` on success
 *      - `ESP_ERR_INVALID_ARG` if `node` or `capability` is invalid
 *      - `ESP_ERR_INVALID_STATE` if a session is active, a connect or
 *        disconnect request is already in progress, or destroy has begun
 *      - `ESP_ERR_NO_MEM` if the registry is full or the capability copy
 *        cannot be allocated
 */
esp_err_t esp_openclaw_node_register_capability(
    esp_openclaw_node_handle_t node,
    const char *capability);

/** @brief Advertise an operator scope before connecting this client instance. */
esp_err_t esp_openclaw_node_register_scope(
    esp_openclaw_node_handle_t node,
    const char *scope);

/**
 * @brief Register a handler for one OpenClaw command.
 *
 * @param[in] node Node handle.
 * @param[in] command Command registration entry.
 *
 * @return
 *      - `ESP_OK` on success
 *      - `ESP_ERR_INVALID_ARG` if `node` or `command` is invalid
 *      - `ESP_ERR_INVALID_STATE` if a session is active, a connect or
 *        disconnect request is already in progress, or destroy has begun
 *      - `ESP_ERR_NO_MEM` if the registry is full or the command name copy
 *        cannot be allocated
 */
esp_err_t esp_openclaw_node_register_command(
    esp_openclaw_node_handle_t node,
    const esp_openclaw_node_command_t *command);

/* Async Request APIs */

/**
 * @brief Request one connect attempt using an explicit caller-chosen source.
 *
 * The request source remains explicit at the API boundary:
 * - saved reconnect session
 * - setup code
 * - gateway token
 * - gateway password
 * - no-auth
 *
 * @param[in] node Node handle.
 * @param[in] request Connect request to submit.
 *
 * @return
 *      - `ESP_OK` if the request was accepted into the component queue
 *      - `ESP_ERR_INVALID_ARG` if `node` or `request` is invalid
 *      - `ESP_ERR_INVALID_STATE` if a session is already active, another
 *        connect or disconnect request is already in progress, the saved
 *        reconnect session is missing for `SAVED_SESSION`, or destroy has begun
 *      - `ESP_ERR_NO_MEM` if the request could not be copied or queued
 *      - `ESP_FAIL` on an unexpected local submission failure
 */
esp_err_t esp_openclaw_node_request_connect(
    esp_openclaw_node_handle_t node,
    const esp_openclaw_node_connect_request_t *request);

/**
 * @brief Request disconnect of the active session.
 *
 * @param[in] node Node handle.
 *
 * @return
 *      - `ESP_OK` if the request was accepted into the component queue
 *      - `ESP_ERR_INVALID_ARG` if `node` is `NULL`
 *      - `ESP_ERR_INVALID_STATE` if no active session is present or destroy has
 *        begun
 *      - `ESP_ERR_NO_MEM` if the request could not be queued
 *      - `ESP_FAIL` on an unexpected local submission failure
 */
esp_err_t esp_openclaw_node_request_disconnect(esp_openclaw_node_handle_t node);

/**
 * @brief Send one asynchronous RPC over a ready Gateway session.
 *
 * The callback runs on the component task and must remain short and
 * non-blocking. `params_json` must contain a JSON object or may be `NULL` to
 * send an empty object. Result strings are valid only during the callback.
 */
esp_err_t esp_openclaw_node_gateway_request(
    esp_openclaw_node_handle_t node,
    const char *method,
    const char *params_json,
    esp_openclaw_node_gateway_request_cb_t callback,
    void *user_ctx);

/* Inspection APIs */

/**
 * @brief Return the stable device identifier for this node instance.
 *
 * @param[in] node Node handle.
 *
 * @return Device identifier string, or `NULL` if @p node is `NULL`.
 */
const char *esp_openclaw_node_get_device_id(esp_openclaw_node_handle_t node);

/**
 * @brief Query whether a saved reconnect session is currently available.
 *
 * @param[in] node Node handle.
 *
 * @return `true` when a saved reconnect session is present.
 */
bool esp_openclaw_node_has_saved_session(esp_openclaw_node_handle_t node);

#ifdef __cplusplus
}
#endif
