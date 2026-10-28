#!/usr/bin/env python3
"""Render the real room UI with LVGL, without ESP-IDF, networking, or hardware."""

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


SDK_HEADERS = {
    "esp_wifi.h": "typedef struct { int unused; } wifi_ap_record_t;",
    "esp_err.h": """
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_TIMEOUT 3
#define ESP_ERR_NO_MEM 4
""",
    "esp_codec_dev.h": "typedef void *esp_codec_dev_handle_t;",
    "esp_openclaw_node.h": """
#include <stddef.h>
#include "esp_err.h"
typedef struct esp_openclaw_node *esp_openclaw_node_handle_t;
typedef struct { const char *code; const char *message; } esp_openclaw_node_error_t;
typedef esp_err_t (*esp_openclaw_node_command_handler_t)(esp_openclaw_node_handle_t,
    void *, const char *, size_t, char **, esp_openclaw_node_error_t *);
typedef struct {
    const char *name;
    esp_openclaw_node_command_handler_t handler;
    void *context;
} esp_openclaw_node_command_t;
esp_err_t esp_openclaw_node_register_command(esp_openclaw_node_handle_t,
    const esp_openclaw_node_command_t *);
""",
    "esp_log.h": """
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
""",
    "esp_timer.h": """
#include <stdint.h>
#include "esp_err.h"
typedef struct ui_timer *esp_timer_handle_t;
typedef struct { void (*callback)(void *); void *arg; const char *name; } esp_timer_create_args_t;
esp_err_t esp_timer_create(const esp_timer_create_args_t *, esp_timer_handle_t *);
esp_err_t esp_timer_stop(esp_timer_handle_t);
esp_err_t esp_timer_start_once(esp_timer_handle_t, uint64_t);
int64_t esp_timer_get_time(void);
""",
    "freertos/FreeRTOS.h": """
#include <stddef.h>
typedef unsigned TickType_t;
typedef unsigned portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
void ui_enter_critical(portMUX_TYPE *);
void ui_exit_critical(portMUX_TYPE *);
#define taskENTER_CRITICAL(lock) ui_enter_critical(lock)
#define taskEXIT_CRITICAL(lock) ui_exit_critical(lock)
size_t strlcpy(char *, const char *, size_t);
""",
    "freertos/semphr.h": "typedef void *SemaphoreHandle_t;",
    "freertos/task.h": '#include "freertos/FreeRTOS.h"',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lvgl-dir", type=Path, required=True, help="Read-only LVGL 9 source directory")
    parser.add_argument("--cjson-dir", type=Path, help="Read-only cJSON source directory; defaults to the sibling managed component")
    parser.add_argument("--snapshot", type=Path, help="Optional synthetic Tab5 framebuffer in PPM format")
    parser.add_argument("--watch-snapshot", type=Path, help="Optional synthetic watch framebuffer in PPM format")
    args = parser.parse_args()
    lvgl = args.lvgl_dir.resolve()
    cjson = (args.cjson_dir or lvgl.parent / "espressif__cjson/cJSON").resolve()
    if not (cjson / "cJSON.c").is_file():
        parser.error("--cjson-dir must contain cJSON sources")
    if not (lvgl / "src/lvgl.h").exists() and not (lvgl / "lvgl.h").exists():
        parser.error("--lvgl-dir must contain LVGL sources")
    tests = Path(__file__).resolve().parent
    component = tests.parent
    bitmap = (component / "assets/openclaw_lobster.argb8888").read_bytes()
    if len(bitmap) != 180 * 180 * 4:
        parser.error("compiled mascot must contain exactly 180 x 180 ARGB8888 pixels")
    with tempfile.TemporaryDirectory(prefix="room-ui-host-") as directory:
        root = Path(directory)
        for name, contents in SDK_HEADERS.items():
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("#pragma once\n" + contents + "\n")
        # The firmware linker embeds these same bytes; the host linker has no IDF helper.
        pixels = root / "pixels.c"
        pixels.write_text(
            'const unsigned char pixels[] __asm__("_binary_openclaw_lobster_argb8888_start") = {\n'
            + ",".join(str(value) for value in bitmap) + "\n};\n"
        )
        subprocess.run([
            sys.executable, str(component.parents[1] / "scripts/generate_watch_lobster.py"),
            "--output-dir", str(root),
        ], check=True)
        watch_pixels = root / "watch_pixels.c"
        watch_pixels.write_text(
            'const unsigned char watch_pixels[] __asm__("_binary_watch_lobster_argb8888_start") = {\n'
            + ",".join(str(value) for value in (root / "watch_lobster.argb8888").read_bytes())
            + "\n};\n"
        )
        avatars = component.parent / "esp-openclaw-directory"
        subprocess.run([
            sys.executable, str(component.parents[1] / "scripts/generate_agent_avatars.py"),
            "--output-dir", str(root),
        ], check=True)
        avatar_pixels = root / "avatar_pixels.c"
        avatar_pixels.write_text(
            'const unsigned char avatar_pixels[] __asm__("_binary_agent_avatars_argb8888_start") = {\n'
            + ",".join(str(value) for value in (root / "agent_avatars.argb8888").read_bytes())
            + "\n};\n"
        )
        binary = root / "room-ui-test"
        command = [
            "cc", "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
            "-DLV_CONF_SKIP", "-DLV_USE_OS=LV_OS_NONE",
            "-DLV_USE_STDLIB_MALLOC=LV_STDLIB_CLIB", "-DLV_USE_LOG=0",
            "-DLV_FONT_MONTSERRAT_20=1", "-DLV_FONT_MONTSERRAT_28=1",
            "-I", str(root), "-I", str(component), "-I", str(component / "include"),
            "-I", str(lvgl), str(tests / "test_room_ui_controller.c"),
            "-I", str(cjson), str(cjson / "cJSON.c"),
            "-I", str(component.parent / "esp-openclaw-directory/include"),
            "-I", str(component.parent / "esp-openclaw-node-provisioning/include"),
            "-I", str(component.parent / "esp-openclaw-ui/include"),
            "-I", str(component.parent / "esp-openclaw-talk/include"),
            str(component / "room_ui_controller.c"), str(component / "room_board.c"),
            str(component / "room_watch_home.c"),
            str(component.parent / "esp-openclaw-ui/openclaw_status_bar.c"),
            str(component.parent / "esp-openclaw-ui/openclaw_connection_details.c"),
            str(component / "room_watch_navigation.c"), str(component / "room_watch_settings.c"),
            str(component / "room_watch_agents.c"),
            str(component / "room_watch_devices.c"),
            str(avatars / "esp_openclaw_avatar.c"), str(avatar_pixels),
            str(component / "room_watch_conversations.c"),
            str(component / "room_face.c"),
            str(pixels), str(watch_pixels), str(component / "room_lobster.c"),
            *map(str, sorted((lvgl / "src").rglob("*.c"))),
            "-lm", "-o", str(binary),
        ]
        print("Building real room UI + LVGL host renderer", flush=True)
        subprocess.run(command, check=True)
        for scenario in ("tab5", "default-off", "animated", "watch"):
            arguments = [str(binary), scenario]
            if args.snapshot and scenario == "tab5":
                arguments.append(str(args.snapshot.resolve()))
            if args.watch_snapshot and scenario == "watch":
                arguments.append(str(args.watch_snapshot.resolve()))
            subprocess.run(arguments, check=True)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode if error.returncode > 0 else 1)
