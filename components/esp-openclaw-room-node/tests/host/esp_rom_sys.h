#pragma once
#ifdef __APPLE__
/* Device ELF placement is not meaningful in the Mach-O host fixture. */
#undef DRAM_STR
#define DRAM_STR(value) (value)
#endif
int esp_rom_printf(const char *format, ...)
    __attribute__((format(printf, 1, 2)));
