#!/usr/bin/env python3
"""Exercise production AFE feed accounting and bounded timing diagnostics."""

import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    tests = Path(__file__).resolve().parent
    component = tests.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--feed-source", type=Path, default=component / "room_aec_src.c")
    args = parser.parse_args()
    source = args.feed_source.read_text()
    start = source.index("static int audio_aec_feed_data(")
    end = source.index("\nstatic int audio_aec_read_by_vad(", start)
    with tempfile.TemporaryDirectory(prefix="room-audio-diagnostics-") as directory:
        root = Path(directory)
        (root / "freertos").mkdir()
        (root / "freertos/FreeRTOS.h").write_text("""#pragma once
typedef unsigned portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0U
#define taskENTER_CRITICAL(lock) ((void)(lock))
#define taskEXIT_CRITICAL(lock) ((void)(lock))
""")
        (root / "esp_timer.h").write_text(
            "#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n")
        # Compile the producer itself so a return-value conversion at the call
        # site cannot make a correct diagnostics implementation hide frame loss.
        (root / "afe_feed_boundary.h").write_text(source[start:end])
        binary = root / "audio-diagnostics-tests"
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g",
            "-I", str(root), "-I", str(component),
            str(tests / "test_room_audio_diagnostics.c"),
            str(component / "room_diagnostics_data.c"),
            str(component / "room_diagnostics_metrics.c"), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
