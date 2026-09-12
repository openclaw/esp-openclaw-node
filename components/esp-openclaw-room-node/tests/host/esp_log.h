#pragma once
#pragma once
typedef enum { ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO } esp_log_level_t;
void esp_log_level_set(const char *tag, esp_log_level_t level);
esp_log_level_t esp_log_level_get(const char *tag);
void host_log(const char *tag, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
#define ESP_LOGE(...) host_log(__VA_ARGS__)
#define ESP_LOGW(...) host_log(__VA_ARGS__)
#define ESP_LOGI(...) host_log(__VA_ARGS__)
