#!/usr/bin/env python3
"""Exercise the compact workspace at both real display sizes with LVGL."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lvgl-dir", required=True, type=Path)
    parser.add_argument("--cjson-dir", required=True, type=Path)
    parser.add_argument("--snapshot-prefix", type=Path)
    args = parser.parse_args()
    tests = Path(__file__).resolve().parent
    root = tests.parents[2]
    with tempfile.TemporaryDirectory(prefix="compact-workspace-") as temp:
        work = Path(temp)
        for name in ("esp_wifi.h", "esp_err.h", "freertos/FreeRTOS.h", "freertos/semphr.h"):
            path = work / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "wifi_host_sdk.h"\n')
        subprocess.run([sys.executable, str(root / "scripts/generate_agent_avatars.py"), "--output-dir", temp], check=True)
        pixels = work / "pixels.c"
        pixels.write_text('const unsigned char pixels[] __asm__("_binary_agent_avatars_argb8888_start") = {' +
            ','.join(str(x) for x in (work / 'agent_avatars.argb8888').read_bytes()) + '};\n')
        command = ["cc", "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
                   "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-Wno-deprecated-declarations",
                   "-DLV_CONF_SKIP", "-DLV_USE_OS=LV_OS_NONE", "-DLV_USE_LOG=0",
                   "-DLV_USE_STDLIB_MALLOC=LV_STDLIB_CLIB"]
        command += [f"-DLV_FONT_MONTSERRAT_{size}=1" for size in (10, 12, 14, 16, 20, 28)]
        for path in (work, args.lvgl_dir, args.cjson_dir, tests.parent / "main",
                     root / "components/esp-openclaw-node-provisioning/tests/host",
                     root / "components/esp-openclaw-node-provisioning/include",
                     root / "components/esp-openclaw-node/include",
                     root / "components/esp-openclaw-reader/include",
                     root / "components/esp-openclaw-directory/include",
                     root / "components/esp-openclaw-ui/include"):
            command += ["-I", str(path)]
        binary = work / "workspace-test"
        command += [str(tests / "test_compact_workspace.c"), str(tests.parent / "main/compact_workspace.c"),
                    str(tests.parent / "main/compact_input.c"),
                    str(root / "components/esp-openclaw-directory/esp_openclaw_avatar.c"), str(pixels),
                    str(args.cjson_dir / "cJSON.c"), *map(str, sorted((args.lvgl_dir / "src").rglob("*.c"))),
                    "-lm", "-o", str(binary)]
        subprocess.run(command, check=True)
        for width in (160, 320):
            snapshot = [f"{args.snapshot_prefix}-{width}"] if args.snapshot_prefix else []
            subprocess.run([str(binary), str(width), *snapshot], check=True)


if __name__ == "__main__":
    main()
