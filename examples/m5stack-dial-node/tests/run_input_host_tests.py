#!/usr/bin/env python3
"""Drive the production Dial input handler through real LVGL input devices."""

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lvgl-dir", type=Path, required=True)
    parser.add_argument("--snapshot-prefix", type=Path)
    args = parser.parse_args()
    lvgl = args.lvgl_dir.resolve()
    tests = Path(__file__).resolve().parent
    root = tests.parents[2]
    with tempfile.TemporaryDirectory(prefix="dial-input-") as directory:
        for name in ("esp_wifi.h", "esp_err.h", "freertos/FreeRTOS.h"):
            path = Path(directory) / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "wifi_host_sdk.h"\n')
        binary = Path(directory) / "dial-input-test"
        avatars = root / "components/esp-openclaw-directory"
        subprocess.run([
            sys.executable, str(root / "scripts/generate_agent_avatars.py"),
            "--output-dir", directory,
        ], check=True)
        pixels = Path(directory) / "avatar_pixels.c"
        pixels.write_text(
            'const unsigned char pixels[] __asm__("_binary_agent_avatars_argb8888_start") = {\n'
            + ",".join(str(value) for value in (Path(directory) / "agent_avatars.argb8888").read_bytes())
            + "\n};\n"
        )
        command = [
            "cc", "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
            "-DLV_CONF_SKIP", "-DLV_USE_OS=LV_OS_NONE", "-DLV_USE_LOG=0",
            "-DLV_USE_STDLIB_MALLOC=LV_STDLIB_CLIB",
            "-DLV_FONT_MONTSERRAT_16=1", "-DLV_FONT_MONTSERRAT_24=1",
            "-I", directory,
            "-I", str(root / "components/esp-openclaw-node-provisioning/tests/host"),
            "-I", str(root / "components/esp-openclaw-node-provisioning/include"),
            "-I", str(tests.parent / "main"), "-I", str(lvgl),
            "-I", str(root / "components/esp-openclaw-directory/include"),
            "-I", str(root / "components/esp-openclaw-ui/include"),
            str(tests / "test_dial_input.c"), str(tests.parent / "main/dial_input.c"),
            str(tests.parent / "main/dial_home.c"),
            str(root / "components/esp-openclaw-ui/openclaw_status_bar.c"),
            str(root / "components/esp-openclaw-ui/openclaw_connection_details.c"),
            str(tests.parent / "main/dial_action.c"),
            str(tests.parent / "main/dial_navigation.c"),
            str(avatars / "esp_openclaw_avatar.c"), str(pixels),
            *map(str, sorted((lvgl / "src").rglob("*.c"))), "-lm", "-o", str(binary),
        ]
        subprocess.run(command, check=True)
        subprocess.run([str(binary), *([str(args.snapshot_prefix)] if args.snapshot_prefix else [])], check=True)


if __name__ == "__main__":
    main()
