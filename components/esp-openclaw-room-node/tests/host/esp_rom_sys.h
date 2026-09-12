#pragma once
int esp_rom_printf(const char *format, ...)
    __attribute__((format(printf, 1, 2)));
