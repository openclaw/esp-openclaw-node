"""Execute the actual reply and shared-send owners with the configured cJSON."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
NODE = ROOT / "components/esp-openclaw-node"
FIXTURE = Path(__file__).parent / "fixtures/test_node_reply_diagnostics.c"


class ReplyDiagnosticsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cjson = Path(os.environ.get(
            "CJSON_DIR",
            str(NODE / "test_apps/esp_openclaw_node_unity_tests/"
                "managed_components/espressif__cjson/cJSON"),
        ))
        if not (cjson / "cJSON.c").is_file():
            raise RuntimeError("Configured cJSON source missing; set CJSON_DIR")
        temporary = tempfile.TemporaryDirectory(prefix="node-reply-test-")
        cls.addClassCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        source = Path(os.environ.get(
            "NODE_REPLY_SOURCE", str(NODE / "src/esp_openclaw_node_protocol.c"),
        )).read_text()
        start = source.index("}\n\n", source.index("static const char *connect_diagnostic_role")) + 3
        shared = source[start:source.index("\nvoid esp_openclaw_node_fail_pending_requests")]
        reply = source[source.index("static void send_invoke_result("):
                       source.index("\nstatic esp_err_t build_connect_response_session_update(")]
        invoke = source[source.index("static void handle_invoke_request("):
                        source.index("\nstatic bool handle_gateway_response(")]
        (directory / "reply_under_test.inc").write_text(shared + reply + invoke)
        cls.binaries = {}
        for quiet in (0, 1):
            binary = directory / f"reply-{quiet}"
            subprocess.run([
                os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L",
                "-Wall", "-Wextra", "-Werror", "-pthread",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                f"-DTEST_LOG_QUIET={quiet}", "-I", str(directory), "-I", str(cjson),
                str(FIXTURE), str(cjson / "cJSON.c"), "-lm", "-o", str(binary),
            ], check=True, timeout=60)
            cls.binaries[quiet] = binary

    def run_case(self, scenario, quiet=0):
        result = subprocess.run(
            [str(self.binaries[quiet]), scenario],
            capture_output=True, text=True, timeout=20,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_send_results_and_envelope_preservation(self):
        for mode in ("negative", "zero", "short", "full"):
            with self.subTest(mode=mode):
                self.run_case(mode)

    def test_shared_noninvoke_send_classification(self):
        for mode in ("negative", "zero", "short", "full"):
            with self.subTest(mode=mode):
                self.run_case("gateway-" + mode)

    def test_serialization_allocation_failure_and_cleanup(self):
        self.run_case("serialize-failure")

    def test_selection_error_payload_and_secret_exclusion(self):
        for scenario in ("device-info", "other-command", "handler-error", "no-payload"):
            with self.subTest(scenario=scenario):
                self.run_case(scenario)

    def test_blocked_send_marker_and_cleanup_order(self):
        self.run_case("blocked-send")

    def test_quiet_logging_keeps_send_behavior(self):
        self.run_case("full", quiet=1)
        self.run_case("zero", quiet=1)


if __name__ == "__main__":
    unittest.main()
