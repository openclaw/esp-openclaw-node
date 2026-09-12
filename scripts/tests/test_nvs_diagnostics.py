"""Execute the actual SDK/session owners against bounded I/O and logging stubs."""

from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import idf_tab5_compat as compat


ROOT = Path(__file__).resolve().parents[2]
FIXTURES = Path(__file__).parent / "fixtures"
HEADERS = {
    "esp_attr.h": '#pragma once\n#define DRAM_ATTR\n#define DRAM_STR(s) (s)\n',
    "esp_rom_sys.h": '#pragma once\nint esp_rom_printf(const char *format, ...);\n',
    "esp_err.h": """#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_NVS_NOT_FOUND 0x1102
static inline const char *esp_err_to_name(esp_err_t error) { (void)error; return "synthetic-error"; }
""",
    "esp_log.h": """#pragma once
static inline void host_legacy_log(const char *format, ...) { (void)format; }
#define ESP_LOGW(tag, ...) do { (void)(tag); host_legacy_log(__VA_ARGS__); } while (0)
""",
    "esp_check.h": """#pragma once
#define ESP_RETURN_ON_ERROR(call, tag, ...) do { \\
    (void)(tag); esp_err_t e = (call); if (e != ESP_OK) return e; \\
} while (0)
""",
    "nvs.h": """#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef unsigned nvs_handle_t;
#define NVS_READWRITE 1
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
void nvs_close(nvs_handle_t);
esp_err_t nvs_get_u8(nvs_handle_t, const char *, uint8_t *);
esp_err_t nvs_get_str(nvs_handle_t, const char *, char *, size_t *);
esp_err_t nvs_set_u8(nvs_handle_t, const char *, uint8_t);
esp_err_t nvs_set_str(nvs_handle_t, const char *, const char *);
esp_err_t nvs_erase_key(nvs_handle_t, const char *);
esp_err_t nvs_commit(nvs_handle_t);
""",
    "nvs_partition.hpp": """#pragma once
#include <cstddef>
#include <cstdint>
#include "esp_err.h"
#define ESP_ENCRYPT_BLOCK_SIZE 16
struct esp_partition_t { const char *label; uint32_t address, size; bool readonly; };
esp_err_t esp_partition_read_raw(const esp_partition_t *, size_t, void *, size_t);
esp_err_t esp_partition_read(const esp_partition_t *, size_t, void *, size_t);
esp_err_t esp_partition_write_raw(const esp_partition_t *, size_t, const void *, size_t);
esp_err_t esp_partition_write(const esp_partition_t *, size_t, const void *, size_t);
esp_err_t esp_partition_erase_range(const esp_partition_t *, size_t, size_t);
namespace nvs {
class NVSPartition {
    const esp_partition_t *mESPPartition;
public:
    explicit NVSPartition(const esp_partition_t *);
    const char *get_partition_name();
    esp_err_t read_raw(size_t, void *, size_t);
    esp_err_t read(size_t, void *, size_t);
    esp_err_t write_raw(size_t, const void *, size_t);
    esp_err_t write(size_t, const void *, size_t);
    esp_err_t erase_range(size_t, size_t);
    uint32_t get_address();
    uint32_t get_size();
    bool get_readonly();
};
}
""",
}


class NvsDiagnosticsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="nvs-diagnostics-test-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.root = Path(cls.temporary.name)
        for name, content in HEADERS.items():
            (cls.root / name).write_text(content)
        original = (FIXTURES / "nvs_partition.cpp").read_bytes()
        if compat.digest(original) != compat.DIAGNOSTIC_ORIGINAL_SHA256:
            raise AssertionError("SDK fixture must be the exact pinned original")
        source = cls.root / compat.DIAGNOSTIC_SOURCE_PATH
        source.parent.mkdir(parents=True)
        source.write_bytes(original)
        subprocess.run(["git", "apply", str(compat.DIAGNOSTIC_PATCH_PATH)],
                       cwd=cls.root, check=True)
        if compat.digest(source.read_bytes()) != compat.DIAGNOSTIC_PATCHED_SHA256:
            raise AssertionError("SDK fixture patch must produce the approved source")
        common = ["-Wall", "-Wextra", "-Werror", "-I", str(cls.root)]
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++11", "-pthread", *common,
                        str(source), str(FIXTURES / "test_nvs_partition.cpp"),
                        "-o", str(cls.root / "partition")], check=True)
        node = ROOT / "components/esp-openclaw-node"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L",
                        *common, "-I", str(node / "private_include"),
                        str(node / "src/esp_openclaw_node_persisted_session.c"),
                        str(FIXTURES / "test_nvs_session.c"),
                        "-o", str(cls.root / "session")], check=True)

    def test_partition_returns_alignment_and_concurrent_first_capture(self):
        # Each process models a fresh boot and resets the private SDK latch naturally.
        for scenario in ("1", "2", "3", "4", "5", "alignment-read", "alignment-write", "concurrent"):
            with self.subTest(scenario=scenario):
                subprocess.run([str(self.root / "partition"), scenario], check=True)

    def test_session_original_errors_presence_clear_and_canary_privacy(self):
        subprocess.run([str(self.root / "session")], check=True)


if __name__ == "__main__":
    unittest.main()
