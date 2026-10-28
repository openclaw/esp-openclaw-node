#!/usr/bin/env python3
"""Exercise production connection admission, NVS races and lease lifecycle."""

from pathlib import Path
import subprocess
import tempfile


def section(source, start, end):
    return source[source.index(start):source.index(end)]


def main():
    component = Path(__file__).resolve().parents[1]
    control = (component / "src/esp_openclaw_node.c").read_text()
    runtime = (component / "src/esp_openclaw_node_runtime.c").read_text()
    source = (component / "src/esp_openclaw_node_connect_source.c").read_text()
    protocol = (component / "src/esp_openclaw_node_protocol.c").read_text()
    with tempfile.TemporaryDirectory(prefix="connect-lease-test-") as directory:
        work = Path(directory)
        (work / "esp_err.h").write_text("""#pragma once
            typedef int esp_err_t;
            enum { ESP_OK, ESP_FAIL, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE,
                   ESP_ERR_NOT_FOUND, ESP_ERR_NO_MEM };
        """)
        (work / "control.inc").write_text(section(
            control, "bool esp_openclaw_node_connect_lease_matches_locked(",
            "static esp_err_t gateway_request("))
        (work / "completion.inc").write_text(section(
            runtime, "static bool teardown_in_progress(",
            "void esp_openclaw_node_fail_if_connect_timed_out("))
        (work / "handoff.inc").write_text(section(
            protocol, "static esp_err_t persist_handoff_device_token(",
            "static esp_err_t persist_handoff_device_tokens("))
        (work / "finalize.inc").write_text(section(
            protocol, "static connect_response_finalize_result_t finalize_connect_response_success(",
            "static void complete_connect_response_outcome("))
        (work / "admission.inc").write_text("\n".join([
            section(source, "static esp_err_t validate_saved_session_connect_preflight_locked(",
                    "esp_err_t esp_openclaw_node_resolve_active_connect_material_locked("),
            source[source.index("esp_err_t esp_openclaw_node_reserve_connect_request_locked("):],
            section(runtime, "static void rollback_pending_control_locked(",
                    "esp_err_t esp_openclaw_node_submit_gateway_request("),
            section(runtime, "static bool cancel_connect_request_before_start(",
                    "static void handle_request_gateway("),
        ]))
        binary = work / "connect-lease-test"
        subprocess.run([
            "cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-g", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            "-I", str(work), "-I", str(component / "include"),
            str(component / "tests/test_connect_lease.c"), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
