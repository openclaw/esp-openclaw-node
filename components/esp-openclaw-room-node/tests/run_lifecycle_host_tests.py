#!/usr/bin/env python3
"""Run the real room controller and Talk adapter against synthetic boundaries."""

import argparse
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
import tempfile


def main():
    tests = Path(__file__).resolve().parent
    repo = tests.parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf-path", type=Path, default=Path(os.environ.get("IDF_PATH", Path.home() / "esp-idf")))
    parser.add_argument("--webrtc-dir", type=Path, default=repo / "third_party/esp-webrtc-solution")
    parser.add_argument("--managed-components", type=Path, default=repo / "examples/m5stack-tab5-room-node/managed_components")
    parser.add_argument("--cjson-dir", type=Path, help="Defaults to cJSON within --managed-components")
    parser.add_argument("--case", action="append", help="Exact case name; repeat to select multiple (default: all)")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--analyze", action="store_true")
    args = parser.parse_args()
    sdk = args.webrtc_dir.resolve() / "components"
    managed = args.managed_components.resolve()
    idf = args.idf_path.resolve() / "components"
    cjson = (args.cjson_dir or managed / "espressif__cjson/cJSON").resolve()
    includes = [
        tests / "host", tests.parent, tests.parent / "include", cjson,
        repo / "components/esp-openclaw-node/include",
        repo / "components/esp-openclaw-node-provisioning/include",
        repo / "components/esp-openclaw-talk/include",
        sdk / "esp_webrtc/include", sdk / "esp_peer/include", sdk / "av_render/include",
        managed / "espressif__esp_capture/include", managed / "espressif__esp_capture/interface",
        managed / "espressif__esp_codec_dev/include", managed / "espressif__esp_codec_dev/interface",
        managed / "espressif__media_lib_sal/include", managed / "espressif__media_lib_sal/include/port",
        managed / "lvgl__lvgl", idf / "esp_common/include", idf / "esp_http_client/include",
        idf / "esp_timer/include", idf / "esp_hw_support/include", idf / "console",
        idf / "soc/include", idf / "soc/linux/include", idf / "heap/include",
    ]
    for path in [*includes, cjson / "cJSON.c"]:
        if not path.exists():
            parser.error(f"Missing {path}; supply read-only SDK/managed dependency paths")
    with tempfile.TemporaryDirectory(prefix="room-lifecycle-host-") as directory:
        binary = Path(directory) / "room-lifecycle-tests"
        command = [
            "clang" if args.analyze else "cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-DLV_CONF_SKIP=1",
            "-Wall", "-Wextra", "-Werror", "-Wmissing-prototypes",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            "-fno-omit-frame-pointer", "-g", "-ffunction-sections", "-fdata-sections",
        ]
        for include in includes:
            command += ["-I", str(include)]
        command += ["-include", str(tests / "host/room_host_fakes.h")]
        if args.analyze:
            for source in [tests / "test_room_talk_lifecycle.c", tests / "host/room_host_fakes.c",
                           tests / "test_room_host_queue.c"]:
                result = subprocess.run(command + ["--analyze", "-Xanalyzer", "-analyzer-output=text",
                    str(source)], cwd=directory)
                if result.returncode:
                    return result.returncode
            return 0
        link = [str(cjson / "cJSON.c"), "-lm",
                "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"]
        queue_binary = Path(directory) / "room-host-queue-tests"
        queue_command = command + [str(tests / "test_room_host_queue.c"), *link, "-o", str(queue_binary)]
        print("BUILD", shlex.join(queue_command), flush=True)
        subprocess.run(queue_command, check=True)
        subprocess.run([str(queue_binary)], check=True)
        for case, message in [("unsupported", "unsupported boundary room_diagnostics_request_open"),
                              ("out-of-bounds", "queue items fit allocated storage")]:
            result = subprocess.run([str(queue_binary), case], capture_output=True, text=True)
            if result.returncode != -signal.SIGABRT or f"HARNESS/SETUP FAILURE: {message}" not in result.stderr:
                print(f"FAIL {case}: expected boundary abort, got {result.returncode}\n{result.stderr}", file=sys.stderr)
                return 1
            print(f"PASS {case}: expected boundary abort", flush=True)
        command += [
            str(tests / "test_room_talk_lifecycle.c"), str(tests / "host/room_host_fakes.c"),
            str(repo / "components/esp-openclaw-talk/src/esp_openclaw_talk.c"),
            *link, "-o", str(binary),
        ]
        print("BUILD", shlex.join(command), flush=True)
        result = subprocess.run(command)
        if result.returncode:
            return result.returncode
        if args.list:
            return subprocess.run([str(binary), "--list"]).returncode
        cases = args.case or subprocess.check_output([str(binary), "--list"], text=True).splitlines()
        failed = 0
        for case in cases:
            print("RUN", shlex.join([str(binary), "--case", case]), flush=True)
            result = subprocess.run([str(binary), "--case", case])
            print(f"RESULT {case}: exit {result.returncode}", flush=True)
            failed += result.returncode != 0
        print(f"{len(cases)} cases, {failed} failed (failures are not inverted)", flush=True)
        return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
