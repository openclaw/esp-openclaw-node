#!/usr/bin/env python3
"""Compile legacy board initializers against the real public room-node header."""

from pathlib import Path
import subprocess
import tempfile


# These dependencies only contribute opaque handle/error types to this header.
# Keep the actual audio-port declaration under test, not a copied struct layout.
SDK_TYPES = {
    "esp_codec_dev.h": "typedef void *esp_codec_dev_handle_t;",
    "esp_err.h": "typedef int esp_err_t;",
    "esp_openclaw_node.h": "typedef struct esp_openclaw_node *esp_openclaw_node_handle_t;",
    "lvgl.h": "typedef struct _lv_display_t lv_display_t;",
}

LEGACY_BOARD = r"""
#include <assert.h>
#include <string.h>
#include "esp_openclaw_room_node.h"

static int board_context;

static lv_display_t *start_display(void *ctx) { return ctx; }
static bool lock_display(void *ctx, uint32_t timeout)
{ return ctx == &board_context && timeout == 100; }
static void unlock_display(void *ctx) { assert(ctx == &board_context); }
static esp_err_t brightness(void *ctx, int percent)
{ return ctx == &board_context ? percent : -1; }

static esp_err_t open_audio(void *ctx, esp_openclaw_room_audio_handles_t *handles)
{
    return ctx == &board_context && handles != NULL ? 0 : -1;
}

int main(void)
{
    esp_openclaw_room_audio_port_t audio = {
        open_audio, "MR", 4, 0x3, 75, true, 30.0f, &board_context
    };
    esp_openclaw_room_audio_handles_t handles = {0};
    assert(audio.open(audio.ctx, &handles) == 0);
    assert(strcmp(audio.afe_layout, "MR") == 0);
    assert(audio.record_channels == 4 && audio.channel_mask == 0x3);
    assert(audio.playback_volume == 75);
    assert(audio.configure_input_gain && audio.input_gain_db == 30.0f);
    assert(audio.playback_gain_db == 0.0f);
    esp_openclaw_room_display_port_t display = {
        start_display, NULL, lock_display, unlock_display, brightness,
        1280, 720, 24, false, 50, &board_context
    };
    assert(display.ctx == &board_context && display.idle_brightness == 0);
    assert(display.start(display.ctx) == (lv_display_t *)&board_context);
    assert(display.lock(display.ctx, 100));
    display.unlock(display.ctx);
    assert(display.set_brightness(display.ctx, 0) == 0);
    return 0;
}
"""


def main():
    include = Path(__file__).resolve().parents[1] / "include"
    with tempfile.TemporaryDirectory(prefix="room-audio-port-") as directory:
        root = Path(directory)
        for name, declaration in SDK_TYPES.items():
            (root / name).write_text("#pragma once\n" + declaration + "\n")
        source = root / "legacy_board.c"
        source.write_text(LEGACY_BOARD)
        binary = root / "legacy_board"
        # Standard C zero-initializes omitted trailing members. Do not enable
        # -Wextra's missing-field warning for this deliberately legacy aggregate.
        subprocess.run(
            ["cc", "-std=c11", "-Wall", "-Werror", "-pedantic",
             "-I", str(root), "-I", str(include), str(source), "-o", str(binary)],
            check=True,
        )
        subprocess.run([str(binary)], check=True)
    print("room audio/display-port positional compatibility test passed")


if __name__ == "__main__":
    main()
