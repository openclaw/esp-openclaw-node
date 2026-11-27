"""Exercise the production WebSocket receive callback with SDK event chunks."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class NodeReceiveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        temporary = tempfile.TemporaryDirectory(prefix="node-receive-")
        cls.addClassCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        source_root = Path(os.environ.get("NODE_SOURCE_ROOT", ROOT))
        source = (source_root / "components/esp-openclaw-node/src/esp_openclaw_node_transport.c").read_text()
        start = source.index("static void enqueue_fatal_message_error(")
        end = source.index("esp_err_t esp_openclaw_node_validate_tls_preflight(", start)
        (directory / "transport_helpers.inc").write_text(source[start:end])
        start = source.index("static esp_err_t local_err_from_ws_event(")
        (directory / "transport_receive.inc").write_text(source[start:])
        source = (source_root / "components/esp-openclaw-node/src/esp_openclaw_node.c").read_text()
        start = source.index("void esp_openclaw_node_clear_data_buffer_locked(")
        end = source.index("void esp_openclaw_node_free_work_message_payload(", start)
        (directory / "transport_clear.inc").write_text(source[start:end])
        cls.binary = directory / "node-receive"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I", str(directory),
            str(Path(__file__).parent / "fixtures/test_node_receive.c"), "-o", str(cls.binary),
        ], check=True, timeout=60)

    def test_receive_events(self):
        for scenario in ("valid", "allocation", "allocation-continuation", "oversize", "malformed",
                         "zero-first", "zero-middle", "empty-frames", "exact-limit", "recovery", "fatal-handoff",
                         "bad-offset", "changed-empty-header"):
            with self.subTest(scenario=scenario):
                result = subprocess.run([str(self.binary), scenario], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
