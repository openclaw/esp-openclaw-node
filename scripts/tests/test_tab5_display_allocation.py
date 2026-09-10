"""Exercise the production ST7121 display configuration for both encryption states."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "examples/m5stack-tab5-room-node/components/tab5_room_board/tab5_room_board.c"
FIXTURE = Path(__file__).parent / "fixtures/test_tab5_display_allocation.c"


class DisplayAllocationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = Path(os.environ.get("TAB5_DISPLAY_SOURCE", SOURCE)).read_text()
        owner = source.index("static lv_display_t *start_st7121_display(void)")
        start = source.index("    lvgl_port_cfg_t port_cfg =", owner)
        end = source.index("\n    esp_lcd_panel_io_handle_t touch_io", start)
        temporary = tempfile.TemporaryDirectory(prefix="tab5-display-allocation-")
        cls.addClassCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        (directory / "display_config_under_test.inc").write_text(source[start:end])
        cls.binary = directory / "display-allocation"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            "-I", str(directory), str(FIXTURE), "-o", str(cls.binary),
        ], check=True, timeout=60)

    def run_case(self, scenario):
        result = subprocess.run(
            [str(self.binary), scenario], capture_output=True, text=True, timeout=20,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_encrypted_display_retains_dma_allocation(self):
        self.run_case("encrypted")

    def test_unencrypted_display_keeps_psram_allocation(self):
        self.run_case("unencrypted")


if __name__ == "__main__":
    unittest.main()
