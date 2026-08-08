#pragma once

#include <stddef.h>
#include <stdint.h>

/** Map signed 16-bit PCM to a speech-useful, bounded 0..100 meter. */
uint8_t room_diagnostics_pcm_level(
    const int16_t *samples,
    size_t sample_count,
    size_t channel_stride);

