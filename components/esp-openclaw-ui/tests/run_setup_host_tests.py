#!/usr/bin/env python3
"""Exercise the shared setup presenter with real LVGL rendering and inputs."""

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lvgl-dir", type=Path, required=True)
    parser.add_argument("--snapshots", type=Path, help="Existing directory for two synthetic Watch PPMs")
    parser.add_argument("--without-qr", action="store_true", help="Exercise manual-only LVGL builds")
    args = parser.parse_args()
    tests = Path(__file__).resolve().parent
    component = tests.parent
    provisioning = component.parent / "esp-openclaw-node-provisioning"
    lvgl = args.lvgl_dir.resolve()
    with tempfile.TemporaryDirectory(prefix="setup-ui-") as directory:
        work = Path(directory)
        for name in ("esp_err.h", "esp_wifi.h", "freertos/FreeRTOS.h"):
            p = work / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text('#include "wifi_host_sdk.h"\n')
        command = ["cc", "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
                   "-fno-sanitize-recover=all", "-DLV_CONF_SKIP", "-DLV_USE_OS=LV_OS_NONE",
                   "-DLV_USE_STDLIB_MALLOC=LV_STDLIB_CLIB", "-DLV_USE_LOG=0",
                   f"-DLV_USE_QRCODE={0 if args.without_qr else 1}", "-DLV_FONT_MONTSERRAT_28=1",
                   "-I", str(work), "-I", str(lvgl), "-I", str(component / "include"),
                   "-I", str(provisioning / "include"), "-I", str(provisioning / "tests/host"),
                   "-I", str(component.parent / "esp-openclaw-node/include"),
                   str(component / "openclaw_setup.c"), str(tests / "test_setup.c"),
                   *map(str, sorted((lvgl / "src").rglob("*.c"))), "-lm", "-o", str(work / "test")]
        print("Building shared setup + actual LVGL", flush=True)
        subprocess.run(command, check=True)
        for profile in ("watch", "dial"):
            arguments = [str(work / "test"), profile]
            if args.snapshots and profile == "watch": arguments.append(str(args.snapshots.resolve()))
            subprocess.run(arguments, check=True, timeout=30)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode if error.returncode > 0 else 1)
