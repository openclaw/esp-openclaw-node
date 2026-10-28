"""Run the production connect/start/worker path with a deterministic clock."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class NodeConnectTimeoutTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        temporary = tempfile.TemporaryDirectory(prefix="node-connect-timeout-")
        cls.addClassCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        source_root = Path(os.environ.get("NODE_SOURCE_ROOT", ROOT))
        sources = source_root / "components/esp-openclaw-node/src"
        selections = (
            ("esp_openclaw_node.c", "void esp_openclaw_node_clear_session_wait_state_locked(", "void esp_openclaw_node_set_pending_control_locked("),
            ("esp_openclaw_node_transport.c", "esp_err_t esp_openclaw_node_start_transport_for_active_source(", "void esp_openclaw_node_send_challenge_kick_ping("),
            ("esp_openclaw_node_runtime.c", "bool esp_openclaw_node_cancel_active_connect_if_requested(", "esp_err_t esp_openclaw_node_enqueue_work_message("),
            ("esp_openclaw_node_runtime.c", "static bool cancel_connect_request_before_start(", "static void handle_request_disconnect("),
            ("esp_openclaw_node_runtime.c", "void esp_openclaw_node_task(", "static void rollback_pending_control_locked("),
        )
        for index, (filename, start, end) in enumerate(selections):
            source = (sources / filename).read_text()
            (directory / f"production_{index}.inc").write_text(
                source[source.index(start):source.index(end)]
            )
        control = (sources / "esp_openclaw_node.c").read_text()
        start = control.index("bool esp_openclaw_node_connect_cancel_pending_locked(")
        end = control.index("esp_err_t esp_openclaw_node_connect_cancel(", start)
        (directory / "cancel_predicates.inc").write_text(control[start:end])
        constants = (source_root / "components/esp-openclaw-node/private_include/esp_openclaw_node_internal.h").read_text()
        (directory / "timeout_constant.inc").write_text(next(
            line for line in constants.splitlines()
            if line.startswith("#define ESP_OPENCLAW_NODE_CONNECT_TIMEOUT_MS ")
        ) + "\n")
        cls.binary = directory / "node-connect-timeout"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            "-I", str(directory),
            str(Path(__file__).parent / "fixtures/test_node_connect_timeout.c"),
            "-o", str(cls.binary),
        ], check=True, timeout=60)

    def test_accepted_connect_deadline(self):
        for scenario in ("idle", "busy", "zero-clock", "ready", "destroying"):
            with self.subTest(scenario=scenario):
                result = subprocess.run(
                    [str(self.binary), scenario],
                    capture_output=True, text=True, timeout=10,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
