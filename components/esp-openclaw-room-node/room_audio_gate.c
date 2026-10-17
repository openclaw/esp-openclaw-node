#include "room_audio_gate.h"

#include <string.h>

#define PLAYBACK_TAIL_US 300000U
#define SILENCE_PEAK 64

void room_audio_gate_init(room_audio_gate_t *gate, const char *mic_layout)
{
    /* NULL has the capture source's default MR layout. */
    gate->enabled = mic_layout != NULL && strchr(mic_layout, 'R') == NULL;
    atomic_init(&gate->playing, false);
    atomic_init(&gate->tail_us, 0);
}

bool room_audio_gate_begin_playback(room_audio_gate_t *gate,
                                    const int16_t *pcm, size_t sample_count)
{
    if (!gate->enabled) return false;
    /* WebRTC can keep rendering silence between replies. Do not let those
     * writes renew the gate and prevent the next user turn. */
    for (size_t i = 0; i < sample_count; ++i) {
        if (pcm[i] > SILENCE_PEAK || pcm[i] < -SILENCE_PEAK) {
            atomic_store(&gate->playing, true);
            return true;
        }
    }
    return false;
}

void room_audio_gate_end_playback(room_audio_gate_t *gate)
{
    if (!gate->enabled) return;
    /* Arm the tail before publishing idle, including failed writes: a driver
     * can accept part of a block before reporting an error. */
    atomic_store(&gate->tail_us, PLAYBACK_TAIL_US);
    atomic_store(&gate->playing, false);
}

void room_audio_gate_filter(room_audio_gate_t *gate, void *pcm, size_t size,
                            unsigned duration_us)
{
    if (gate == NULL || !gate->enabled) return;
    bool mute = atomic_load(&gate->playing);
    if (!mute) {
        unsigned remaining = atomic_load(&gate->tail_us);
        while (remaining > 0) {
            unsigned next = remaining > duration_us ? remaining - duration_us : 0;
            if (atomic_compare_exchange_weak(&gate->tail_us, &remaining, next)) {
                mute = true;
                break;
            }
        }
    }
    if (mute) memset(pcm, 0, size);
}
