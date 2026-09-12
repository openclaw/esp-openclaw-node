#pragma once

void talk_host_log_error(const char *tag, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
void talk_host_log_info(const char *tag, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
#define ESP_LOGE(...) talk_host_log_error(__VA_ARGS__)
#define ESP_LOGI(...) talk_host_log_info(__VA_ARGS__)
