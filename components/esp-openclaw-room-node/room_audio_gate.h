#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

typedef struct {
    bool enabled;
    atomic_bool playing;
    atomic_uint tail_us;
} room_audio_gate_t;

void room_audio_gate_init(room_audio_gate_t *gate, const char *mic_layout);
/* Returns true when a non-silent PCM16 block needs a matching end call. */
bool room_audio_gate_begin_playback(room_audio_gate_t *gate,
                                    const int16_t *pcm, size_t sample_count);
void room_audio_gate_end_playback(room_audio_gate_t *gate);

/* Called once per raw capture block, before AFE/VAD. The capture clock drains
 * the tail even when the block is silent; playback never stops capture. */
void room_audio_gate_filter(room_audio_gate_t *gate, void *pcm, size_t size,
                            unsigned duration_us);
