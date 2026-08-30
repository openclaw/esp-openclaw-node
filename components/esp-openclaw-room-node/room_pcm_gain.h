#pragma once

#include <stddef.h>
#include <stdint.h>

#define ROOM_PCM_GAIN_UNITY_Q12 4096U

/* The media owner validates finite 0..12 dB before conversion. */
uint32_t room_pcm_gain_q12_from_db(float gain_db);
/* Gain is at most +12 dB (or its reciprocal for tone headroom), so the
 * signed 32-bit product plus rounding fits even at full-scale PCM16. */
void room_pcm_gain_apply_s16(int16_t *samples, size_t sample_count, uint32_t gain_q12);
