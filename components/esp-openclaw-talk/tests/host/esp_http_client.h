#pragma once

#include "esp_err.h"

typedef struct esp_http_client *esp_http_client_handle_t;
typedef enum { HTTP_EVENT_ON_DATA = 4 } esp_http_client_event_id_t;
typedef enum { HTTP_METHOD_POST = 1 } esp_http_client_method_t;
typedef struct esp_http_client_event {
    esp_http_client_event_id_t event_id;
    void *user_data;
    void *data;
    int data_len;
} esp_http_client_event_t;
typedef struct {
    const char *url;
    esp_http_client_method_t method;
    int timeout_ms;
    esp_err_t (*event_handler)(esp_http_client_event_t *event);
    void *user_data;
    int buffer_size;
    int buffer_size_tx;
} esp_http_client_config_t;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client, const char *data, int length);
esp_err_t esp_http_client_perform(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);
