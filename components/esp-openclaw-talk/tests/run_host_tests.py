#!/usr/bin/env python3
"""Execute the existing Talk Unity cases against their included production C."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    tests = Path(__file__).resolve().parent
    repo = tests.parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf-path", type=Path, default=Path(os.environ.get("IDF_PATH", Path.home() / "esp-idf")))
    parser.add_argument("--cjson-dir", type=Path, default=repo / "components/esp-openclaw-node/test_apps/esp_openclaw_node_unity_tests/managed_components/espressif__cjson/cJSON")
    parser.add_argument("--filter", default="")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--sanitize", action="store_true")
    mode.add_argument("--analyze", action="store_true", help="Analyze the real Talk test/production translation unit with Clang")
    args = parser.parse_args()
    unity = args.idf_path.resolve() / "components/unity/unity/src"
    cjson = args.cjson_dir.resolve()
    for source in [unity / "unity.c", cjson / "cJSON.c"]:
        if not source.is_file():
            parser.error(f"Missing {source}; provide an installed ESP-IDF and configured test-app cJSON sources")
    includes = [
        tests / "host", tests.parent / "include", unity, cjson,
        repo / "components/esp-openclaw-node/include",
        repo / "third_party/esp-webrtc-solution/components/esp_webrtc/include",
        repo / "third_party/esp-webrtc-solution/components/esp_peer/include",
    ]
    with tempfile.TemporaryDirectory(prefix="openclaw-talk-host-") as directory:
        binary = Path(directory) / "talk-tests"
        command = ["clang" if args.analyze else "cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-DOPENCLAW_TALK_HOST_TEST=1", "-Wall", "-Wextra", "-Werror"]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g"]
        for include in includes:
            command += ["-I", str(include)]
        command += ["-include", str(tests / "host/talk_host_runner.h")]
        if args.analyze:
            command += ["--analyze", "-Xanalyzer", "-analyzer-output=text", str(tests / "test_esp_openclaw_talk.c")]
            return subprocess.run(command, cwd=directory).returncode
        command += [
            str(tests / "test_esp_openclaw_talk.c"),
            str(tests / "host/talk_host_runner.c"),
            str(unity / "unity.c"), str(cjson / "cJSON.c"),
            "-lm", "-o", str(binary),
        ]
        subprocess.run(command, check=True)
        return subprocess.run([str(binary), args.filter]).returncode


if __name__ == "__main__":
    raise SystemExit(main())
