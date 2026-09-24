"""Execute production completion helpers with synthetic transport boundaries."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class NodeTeardownTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        temporary = tempfile.TemporaryDirectory(prefix="node-teardown-")
        cls.addClassCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        source = Path(os.environ.get(
            "NODE_RUNTIME_SOURCE",
            str(ROOT / "components/esp-openclaw-node/src/esp_openclaw_node_runtime.c"),
        )).read_text()
        start = source.index('\n', source.index('#include "esp_timer.h"'))
        end = source.index("\nvoid esp_openclaw_node_fail_if_connect_timed_out(")
        (directory / "completion_under_test.inc").write_text(source[start:end])
        cls.binary = directory / "node-teardown"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-pthread", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            "-I", str(directory),
            str(Path(__file__).parent / "fixtures/test_node_teardown.c"),
            "-o", str(cls.binary),
        ], check=True, timeout=60)

    def test_completion_states_and_cleanup_race(self):
        for completion in ("disconnect", "connect-failure"):
            for scenario in ("normal", "destroying", "closed", "during-cleanup"):
                with self.subTest(completion=completion, scenario=scenario):
                    result = subprocess.run(
                        [str(self.binary), completion, scenario],
                        capture_output=True, text=True, timeout=10,
                    )
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
