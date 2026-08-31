#!/usr/bin/env python3
"""Barrier-driven lifetime proofs using real Talk/Node/HTTP headers and pthreads."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

CASES = ["ice-drain", "connected-drain", "failure-drain", "answer-drain", "late-http",
         "late-create", "late-config", "late-failure", "submission-failure", "canceled-start", "lifecycle-isolation", "admission-race"]


def main():
    tests = Path(__file__).resolve().parent
    repo = tests.parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf-path", type=Path, default=Path(os.environ.get("IDF_PATH", Path.home() / "esp-idf")))
    parser.add_argument("--webrtc-dir", type=Path, default=repo / "third_party/esp-webrtc-solution")
    parser.add_argument("--cjson-dir", type=Path, default=repo / "components/esp-openclaw-node/test_apps/esp_openclaw_node_unity_tests/managed_components/espressif__cjson/cJSON")
    parser.add_argument("--tsan", action="store_true")
    parser.add_argument("--analyze", action="store_true")
    args = parser.parse_args()
    idf = args.idf_path.resolve() / "components"
    sdk = args.webrtc_dir.resolve() / "components"
    includes = [idf / "esp_http_client/include", idf / "esp_common/include", tests / "host",
                repo / "components/esp-openclaw-room-node/tests/host", tests.parent / "include",
                repo / "components/esp-openclaw-node/include", sdk / "esp_webrtc/include",
                sdk / "esp_peer/include", args.cjson_dir.resolve()]
    command = ["clang" if args.analyze else "cc", "-std=c11", "-pthread", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra",
               "-Werror", "-Wmissing-prototypes", "-g", "-fno-omit-frame-pointer"]
    for include in includes:
        command += ["-I", str(include)]
    with tempfile.TemporaryDirectory(prefix="talk-threaded-") as directory:
        if args.analyze:
            return subprocess.run(command + ["--analyze", "-Xanalyzer", "-analyzer-output=text",
                str(tests / "test_talk_threaded.c")], cwd=directory).returncode
        binary = Path(directory) / "threaded-tests"
        command += ["-fsanitize=thread" if args.tsan else "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        compat = Path(directory) / "compat"
        subprocess.run(command + [str(tests / "test_config_compat.c"), "-o", str(compat)], check=True)
        subprocess.run([str(compat)], check=True, timeout=20)
        subprocess.run(command + [str(tests / "test_talk_threaded.c"),
            str(args.cjson_dir.resolve() / "cJSON.c"), "-lm", "-o", str(binary)], check=True)
        for case in CASES:
            subprocess.run([str(binary), case], check=True, timeout=20)
    print(f"{len(CASES)} threaded/ownership cases passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
