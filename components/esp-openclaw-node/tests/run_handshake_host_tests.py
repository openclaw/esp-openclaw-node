#!/usr/bin/env python3
"""Validate handshake and RPC response parsing with a constrained heap."""

import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cjson-dir", type=Path, required=True)
    args = parser.parse_args()
    component = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="handshake-test-") as directory:
        work = Path(directory)
        cjson = work / "cjson.o"
        executable = work / "handshake-test"
        subprocess.run([
            "cc", "-std=c11", "-g", "-fsanitize=address,undefined",
            "-Wno-deprecated-declarations", "-c", str(args.cjson_dir / "cJSON.c"),
            "-o", str(cjson),
        ], check=True)
        subprocess.run([
            "cc", "-std=c11", "-g", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            "-I", str(component / "private_include"), "-I", str(component / "include"), "-I", str(args.cjson_dir),
            str(component / "src/esp_openclaw_node_handshake.c"),
            str(component / "tests/test_handshake.c"), str(cjson), "-lm", "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)
        protocol = (component / "src/esp_openclaw_node_protocol.c").read_text()
        callback = protocol[protocol.index("static void invoke_gateway_callback("):
                            protocol.index("void esp_openclaw_node_send_gateway_request(")]
        response = protocol[protocol.index("static bool handle_gateway_response("):
                            protocol.index("static void emit_gateway_event(")]
        (work / "response.inc").write_text(callback + response)
        response_test = work / "response-test"
        subprocess.run([
            "cc", "-std=c11", "-g", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            "-I", str(work), "-I", str(component / "private_include"), "-I", str(component / "include"), "-I", str(args.cjson_dir),
            str(component / "src/esp_openclaw_node_handshake.c"),
            str(component / "tests/test_response.c"), str(cjson), "-lm", "-o", str(response_test),
        ], check=True)
        subprocess.run([str(response_test)], check=True)


if __name__ == "__main__":
    main()
