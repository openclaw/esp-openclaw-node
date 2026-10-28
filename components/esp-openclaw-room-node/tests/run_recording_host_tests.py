#!/usr/bin/env python3
"""Exercise bounded recording and its real media ingress with SDK codec fakes."""

import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    tests = Path(__file__).resolve().parent
    component = tests.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--managed-components", type=Path, required=True)
    parser.add_argument("--analyze", action="store_true")
    args = parser.parse_args()
    managed = args.managed_components.resolve()
    codec = managed / "espressif__esp_audio_codec/include"
    effects = managed / "espressif__esp_audio_effects/include"
    for path in [codec, effects]:
        if not path.is_dir():
            parser.error(f"Missing {path}; supply read-only managed dependencies")
    with tempfile.TemporaryDirectory(prefix="room-recording-") as directory:
        root = Path(directory)
        (root / "freertos").mkdir()
        for name in ["FreeRTOS", "queue", "semphr", "task", "idf_additions"]:
            (root / f"freertos/{name}.h").write_text('#include "recording_host.h"\n')
        for name in ["esp_timer", "esp_heap_caps"]:
            (root / f"{name}.h").write_text('#include "recording_host.h"\n')
        (root / "esp_err.h").write_text("""#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
""")
        # Compile exact media entrypoints, avoiding unrelated board/render stubs.
        media = (component / "room_media.c").read_text()
        sections = []
        for start, end in [
            ("static void room_afe_wake(", "\n/*"),
            ("static void wake_drain_task(", "\nstatic void tone_set_result("),
            ("esp_err_t room_media_begin_talk(", "\nesp_err_t room_media_request_test_tone("),
        ]:
            offset = media.index(start)
            sections.append(media[offset:media.index(end, offset)])
        (root / "media_boundary.h").write_text("\n".join(sections))
        command = ["clang" if args.analyze else "cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
                   "-g", "-pthread", "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        for include in [root, tests / "recording_host", component, effects, codec,
                        codec / "encoder", codec / "encoder/impl"]:
            command += ["-I", str(include)]
        sources = [component / "room_recording.c", tests / "test_room_recording.c"]
        if args.analyze:
            for source in sources:
                subprocess.run(command + ["--analyze", "-Xanalyzer", "-analyzer-output=text", str(source)],
                               cwd=root, check=True)
            return
        obj = root / "recording.o"
        subprocess.run(command + ["-Dfree=recording_host_free", "-c", str(sources[0]), "-o", str(obj)], check=True)
        binary = root / "recording-tests"
        subprocess.run(command + [str(sources[1]), str(obj), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
