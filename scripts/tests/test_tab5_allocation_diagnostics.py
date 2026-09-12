"""Execute the Tab5 callback and real transport-start owner with bounded SDK stubs."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
NODE = ROOT / "components/esp-openclaw-node"
MAIN = ROOT / "examples/m5stack-tab5-room-node/main/main.c"
FIXTURE = Path(__file__).parent / "fixtures/test_tab5_allocation_diagnostics.c"
HEADERS = {
    "esp_err.h": """#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERROR_CHECK(call) do { if ((call) != ESP_OK) abort(); } while (0)
""",
    "esp_attr.h": "#pragma once\n#define IRAM_ATTR\n#define DRAM_ATTR\n",
    "esp_heap_caps.h": """#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef void (*esp_alloc_failed_hook_t)(size_t, uint32_t, const char *);
esp_err_t heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t);
size_t heap_caps_get_free_size(uint32_t);
size_t heap_caps_get_largest_free_block(uint32_t);
""",
    "esp_log.h": """#pragma once
void test_log(const char *, const char *, ...) __attribute__((format(printf, 2, 3)));
#define ESP_LOGI(tag, ...) test_log(tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) test_log(tag, __VA_ARGS__)
""",
    "freertos/FreeRTOS.h": """#pragma once
typedef void *TaskHandle_t;
int xPortInIsrContext(void);
""",
    "freertos/task.h": """#pragma once
#include "freertos/FreeRTOS.h"
TaskHandle_t xTaskGetCurrentTaskHandle(void);
""",
    "esp_openclaw_room_node.h": """#pragma once
#include <stdlib.h>
#include "esp_err.h"
typedef struct { int unused; } esp_openclaw_room_node_config_t;
esp_err_t esp_openclaw_room_node_start(const esp_openclaw_room_node_config_t *);
""",
    "tab5_room_board.h": """#pragma once
#include "esp_openclaw_room_node.h"
esp_err_t tab5_room_board_config(esp_openclaw_room_node_config_t *);
""",
    "forbid_alloc.h": """#include <stddef.h>
void *test_forbidden_malloc(size_t);
void *test_forbidden_calloc(size_t, size_t);
void *test_forbidden_realloc(void *, size_t);
int test_allocator_strncmp(const char *, const char *, size_t);
#define malloc test_forbidden_malloc
#define calloc test_forbidden_calloc
#define realloc test_forbidden_realloc
#define strncmp test_allocator_strncmp
""",
}


class AllocationDiagnosticsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        temporary = tempfile.TemporaryDirectory(prefix="tab5-allocation-test-")
        cls.addClassCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        for name, content in HEADERS.items():
            path = directory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        transport = (NODE / "src/esp_openclaw_node_transport.c").read_text()
        start = transport.index("esp_err_t esp_openclaw_node_start_transport_for_active_source(")
        end = transport.index("\nvoid esp_openclaw_node_send_challenge_kick_ping(", start)
        (directory / "transport_start_under_test.inc").write_text(transport[start:end])
        start = transport.index("__attribute__((weak)) void esp_openclaw_node_transport_start_begin(")
        end = transport.index("\nstatic void websocket_event_handler(", start)
        (directory / "default_hooks.c").write_text(
            '#include "esp_openclaw_node_transport_diag.h"\n' + transport[start:end]
        )
        common = [
            os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L",
            "-Wall", "-Wextra", "-Werror", "-pthread",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            "-I", str(directory), "-I", str(NODE / "private_include"),
        ]
        subprocess.run([
            *common, "-include", str(directory / "forbid_alloc.h"), "-c",
            os.environ.get("TAB5_ALLOCATION_MAIN", str(MAIN)), "-o", str(directory / "tab5.o"),
        ], check=True, timeout=60)
        cls.binary = directory / "allocation-test"
        subprocess.run([
            *common, str(FIXTURE), str(directory / "tab5.o"),
            str(directory / "default_hooks.c"), "-o", str(cls.binary),
        ], check=True, timeout=60)
        # Link defaults independently, as non-Tab5 applications do.
        (directory / "noop.c").write_text(
            '#include "esp_openclaw_node_transport_diag.h"\n'
            'int main(void) { esp_openclaw_node_transport_start_begin("node"); '
            'esp_openclaw_node_transport_start_end("node", ESP_FAIL); return 0; }\n'
        )
        cls.noop = directory / "noop"
        subprocess.run([
            *common, str(directory / "noop.c"), str(directory / "default_hooks.c"),
            "-o", str(cls.noop),
        ], check=True, timeout=60)

    def run_case(self, scenario):
        result = subprocess.run(
            [str(self.binary), scenario], capture_output=True, text=True, timeout=20,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_first_capture_and_post_return_heap_phase(self):
        self.run_case("capture")

    def test_success_after_capture_and_failure_without_capture(self):
        for scenario in ("captured-success", "uncaptured-failure", "success"):
            with self.subTest(scenario=scenario):
                self.run_case(scenario)

    def test_isr_unrelated_task_unknown_allocator_and_disarm(self):
        self.run_case("exclusions")

    def test_role_isolation_and_concurrent_first_capture(self):
        self.run_case("concurrent")

    def test_rearming_and_original_transport_error_cleanup(self):
        self.run_case("rearm")
        for scenario in ("init-failure", "register-failure", "no-source"):
            with self.subTest(scenario=scenario):
                self.run_case(scenario)

    def test_registration_before_board_and_nonfatal_registration_failure(self):
        self.run_case("install-failure")

    def test_other_boards_link_default_noop_hooks(self):
        subprocess.run([str(self.noop)], check=True, timeout=20)
        for binary, weak in ((self.binary, False), (self.noop, True)):
            symbols = subprocess.run(
                ["nm", "-m" if sys.platform == "darwin" else "-g", str(binary)],
                check=True, capture_output=True, text=True, timeout=20,
            ).stdout.splitlines()
            for hook in (
                "esp_openclaw_node_transport_start_begin",
                "esp_openclaw_node_transport_start_end",
            ):
                name = "_" + hook if sys.platform == "darwin" else hook
                matches = [line for line in symbols if line.split()[-1:] == [name]]
                self.assertEqual(len(matches), 1, symbols)
                if sys.platform == "darwin":
                    self.assertIn("external", matches[0])
                    self.assertEqual("weak external" in matches[0], weak, matches[0])
                else:
                    self.assertEqual(matches[0].split()[-2], "W" if weak else "T")


if __name__ == "__main__":
    unittest.main()
