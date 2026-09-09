"""Execute the real capture owner against bounded V4L2 and logging stubs."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "examples/m5stack-tab5-room-node/components/tab5_room_board/tab5_room_board.c"
FIXTURE = Path(__file__).parent / "fixtures/test_tab5_camera_capture.c"


class CameraCaptureDiagnosticsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = Path(os.environ.get("TAB5_CAMERA_SOURCE", SOURCE)).read_text()
        start = source.index("typedef struct {\n    int fd;")
        end = source.index("\ntypedef struct {", start + 1)
        temporary = tempfile.TemporaryDirectory(prefix="tab5-camera-capture-")
        cls.addClassCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        (directory / "camera_capture_under_test.inc").write_text(source[start:end])
        cls.binary = directory / "camera-capture"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            "-I", str(directory), str(FIXTURE), "-o", str(cls.binary),
        ], check=True)

    def run_case(self, scenario):
        result = subprocess.run([str(self.binary), scenario], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_failure_stage_errno_domain_and_partial_cleanup(self):
        for scenario in (
            "bsp", "open", "gfmt", "default-size", "sfmt", "gfmt-after",
            "negotiated-size", "negotiated-format", "negotiated-stride",
            "reqbufs", "buffer-count", "query0", "query1", "buffer-length",
            "mmap0", "mmap1", "qbuf0", "qbuf1", "streamon", "dqbuf",
            "frame-index", "requeue", "frame-length",
        ):
            with self.subTest(scenario=scenario):
                self.run_case(scenario)

    def test_success_formats_stride_and_bounded_loop_marker(self):
        for scenario in ("success", "rgb565x", "explicit-stride", "zero-bytesused", "delayed"):
            with self.subTest(scenario=scenario):
                self.run_case(scenario)

    def test_existing_initialization_cache_policy(self):
        for scenario in ("bsp-retry", "open-cache", "success-cache"):
            with self.subTest(scenario=scenario):
                self.run_case(scenario)


if __name__ == "__main__":
    unittest.main()
