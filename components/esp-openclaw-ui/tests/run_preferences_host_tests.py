#!/usr/bin/env python3
"""Exercise production preference owners across process restarts and NVS failures."""
from pathlib import Path
import subprocess
import tempfile

tests = Path(__file__).resolve().parent
component = tests.parent
with tempfile.TemporaryDirectory(prefix="ui-preferences-") as directory:
    root = Path(directory)
    (root / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_ERR_INVALID_ARG 1\n")
    (root / "lvgl.h").write_text("#pragma once\ntypedef struct { int unused; } lv_font_t;\nextern const lv_font_t lv_font_montserrat_14;\n")
    (root / "font.c").write_text('#include "lvgl.h"\nconst lv_font_t lv_font_montserrat_14 = {0};\n')
    binary = root / "preferences"
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-I", str(root), "-I", str(tests / "host"), "-I", str(component / "include"),
        str(tests / "test_preferences.c"), str(tests / "host/nvs.c"), str(root / "font.c"),
        *[str(component / name) for name in ("openclaw_preferences.c", "openclaw_reading.c", "openclaw_density.c")],
        "-o", str(binary),
    ], check=True)
    for phase in ("write", "reboot", "corrupt"):
        subprocess.run([str(binary), phase, str(root / "nvs")], check=True)
    print("Preference persistence, restart, corruption, and storage failure cases passed")
