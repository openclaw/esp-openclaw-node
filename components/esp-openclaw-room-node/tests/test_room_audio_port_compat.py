#!/usr/bin/env python3
"""Compile a legacy board initializer against the real public room-node header."""

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
    print("room audio-port positional compatibility test passed")


if __name__ == "__main__":
    main()
