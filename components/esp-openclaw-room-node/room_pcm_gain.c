#include "room_pcm_gain.h"

#include <limits.h>
#include <math.h>

uint32_t room_pcm_gain_q12_from_db(float gain_db)
{
    float linear = powf(10.0f, gain_db / 20.0f);
    return (uint32_t)(linear * ROOM_PCM_GAIN_UNITY_Q12 + 0.5f);
}

void room_pcm_gain_apply_s16(int16_t *samples, size_t sample_count, uint32_t gain_q12)
{
    if (samples == NULL || sample_count == 0 || gain_q12 == ROOM_PCM_GAIN_UNITY_Q12) return;

    for (size_t i = 0; i < sample_count; ++i) {
        int32_t scaled = (int32_t)samples[i] * (int32_t)gain_q12;
        scaled = scaled >= 0
            ? (scaled + (int32_t)ROOM_PCM_GAIN_UNITY_Q12 / 2) /
                (int32_t)ROOM_PCM_GAIN_UNITY_Q12
            : (scaled - (int32_t)ROOM_PCM_GAIN_UNITY_Q12 / 2) /
                (int32_t)ROOM_PCM_GAIN_UNITY_Q12;
        if (scaled > INT16_MAX) scaled = INT16_MAX;
        else if (scaled < INT16_MIN) scaled = INT16_MIN;
        samples[i] = (int16_t)scaled;
    }
}
