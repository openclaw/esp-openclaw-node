#!/usr/bin/env python3
"""Drive the production Dial input handler through real LVGL input devices."""

import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lvgl-dir", type=Path, required=True)
    args = parser.parse_args()
    lvgl = args.lvgl_dir.resolve()
    tests = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="dial-input-") as directory:
        binary = Path(directory) / "dial-input-test"
        command = [
            "cc", "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
            "-DLV_CONF_SKIP", "-DLV_USE_OS=LV_OS_NONE", "-DLV_USE_LOG=0",
            "-DLV_USE_STDLIB_MALLOC=LV_STDLIB_CLIB",
            "-DLV_FONT_MONTSERRAT_16=1",
            "-I", str(tests.parent / "main"), "-I", str(lvgl),
            str(tests / "test_dial_input.c"), str(tests.parent / "main/dial_input.c"),
            str(tests.parent / "main/dial_home.c"),
            *map(str, sorted((lvgl / "src").rglob("*.c"))), "-lm", "-o", str(binary),
        ]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
