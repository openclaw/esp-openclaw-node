#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include "nvs_partition.hpp"

static std::atomic<int> calls{0}, records{0};
static int result, recorded_error;
static unsigned recorded_operation;
static thread_local bool in_io;
static const esp_partition_t partition = {"synthetic-id-canary", 4096, 8192, false};

int esp_rom_printf(const char *format, ...)
{
    assert(!in_io);
    va_list args;
    va_start(args, format);
    recorded_operation = va_arg(args, unsigned);
    recorded_error = va_arg(args, int);
    va_end(args);
    char text[128];
    std::snprintf(text, sizeof(text), format, recorded_operation, recorded_error);
    assert(std::strstr(text, "canary") == nullptr);
    records.fetch_add(1);
    return 0;
}

static int io(const esp_partition_t *p, size_t offset, size_t size)
{
    in_io = true;
    assert(p == &partition && offset == 32 && size == 16);
    calls.fetch_add(1);
    in_io = false;
    return result;
}
esp_err_t esp_partition_read_raw(const esp_partition_t *p, size_t o, void *, size_t s) { return io(p, o, s); }
esp_err_t esp_partition_read(const esp_partition_t *p, size_t o, void *, size_t s) { return io(p, o, s); }
esp_err_t esp_partition_write_raw(const esp_partition_t *p, size_t o, const void *, size_t s) { return io(p, o, s); }
esp_err_t esp_partition_write(const esp_partition_t *p, size_t o, const void *, size_t s) { return io(p, o, s); }
esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t o, size_t s) { return io(p, o, s); }

static int invoke(nvs::NVSPartition &nvs, unsigned op, size_t size = 16)
{
    char buffer[32] = "synthetic-token-canary";
    switch (op) {
    case 1: return nvs.read_raw(32, buffer, size);
    case 2: return nvs.read(32, buffer, size);
    case 3: return nvs.write_raw(32, buffer, size);
    case 4: return nvs.write(32, buffer, size);
    default: return nvs.erase_range(32, size);
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    nvs::NVSPartition nvs(&partition);
    assert(nvs.get_address() == 4096 && nvs.get_size() == 8192 && !nvs.get_readonly());
    if (std::strncmp(argv[1], "alignment-", 10) == 0) {
        unsigned op = std::strcmp(argv[1], "alignment-read") == 0 ? 2 : 4;
        assert(invoke(nvs, op, 1) == ESP_ERR_INVALID_ARG);
        assert(calls == 0 && records == 1);
        assert(recorded_operation == op && recorded_error == ESP_ERR_INVALID_ARG);
        assert(invoke(nvs, op, 1) == ESP_ERR_INVALID_ARG && records == 1 && calls == 0);
    } else if (std::strcmp(argv[1], "concurrent") == 0) {
        result = -23;
        std::thread workers[16];
        for (auto &worker : workers) worker = std::thread([&] { assert(invoke(nvs, 1) == -23); });
        for (auto &worker : workers) worker.join();
        assert(calls == 16 && records == 1 && recorded_error == -23);
    } else {
        unsigned op = static_cast<unsigned>(std::atoi(argv[1]));
        assert(invoke(nvs, op) == ESP_OK && records == 0 && calls == 1);
        result = -37;
        assert(invoke(nvs, op) == -37 && records == 1 && calls == 2);
        assert(recorded_operation == op && recorded_error == -37);
        result = 4363;
        assert(invoke(nvs, op) == 4363 && records == 1 && calls == 3);
    }
    return 0;
}
