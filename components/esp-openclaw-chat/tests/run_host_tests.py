#!/usr/bin/env python3
"""Run the chat client against real cJSON and Unity, with a controlled Node transport."""

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
    mode.add_argument("--analyze", action="store_true")
    args = parser.parse_args()
    unity = args.idf_path.resolve() / "components/unity/unity/src"
    cjson = args.cjson_dir.resolve()
    for source in [unity / "unity.c", cjson / "cJSON.c"]:
        if not source.is_file():
            parser.error(f"Missing {source}; provide installed ESP-IDF and cJSON sources")
    includes = [tests / "host", tests.parent / "include", unity, cjson,
                repo / "components/esp-openclaw-node/include",
                repo / "components/esp-openclaw-talk/tests/host"]
    with tempfile.TemporaryDirectory(prefix="openclaw-chat-host-") as directory:
        binary = Path(directory) / "chat-tests"
        command = ["clang" if args.analyze else "cc", "-std=c11", "-pthread", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror"]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g"]
        for include in includes:
            command += ["-I", str(include)]
        command += ["-include", str(tests / "host/chat_host_runner.h")]
        source = tests / "test_esp_openclaw_chat.c"
        if args.analyze:
            command += ["--analyze", "-Xanalyzer", "-analyzer-output=text", str(source)]
        else:
            command += [str(source), str(tests / "host/chat_host_runner.c"),
                        str(unity / "unity.c"), str(cjson / "cJSON.c"), "-lm", "-o", str(binary)]
        subprocess.run(command, check=True, cwd=directory)
        return 0 if args.analyze else subprocess.run([str(binary), args.filter]).returncode


if __name__ == "__main__":
    raise SystemExit(main())
