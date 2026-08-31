#pragma once

void talk_host_log_error(const char *tag, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
#define ESP_LOGE(...) talk_host_log_error(__VA_ARGS__)
