#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "room_diagnostics_metrics.h"

int main(void)
{
    const int16_t silence[16] = {0};
    assert(room_diagnostics_pcm_level(silence, 16, 1) == 0);

    int16_t speech[16];
    for (size_t i = 0; i < 16; ++i) speech[i] = (i & 1U) ? 1500 : -1500;
    uint8_t speech_level = room_diagnostics_pcm_level(speech, 16, 1);
    assert(speech_level >= 70 && speech_level <= 90);

    const int16_t quiet_speech[] = {32, -32, 32, -32};
    assert(room_diagnostics_pcm_level(quiet_speech, 4, 1) >= 15);

    const int16_t interleaved[] = {100, 12000, -100, -12000};
    assert(room_diagnostics_pcm_level(interleaved, 4, 2) >= 20);
    assert(room_diagnostics_pcm_level(interleaved + 1, 3, 2) == 100);

    const int16_t full_scale[] = {INT16_MIN, INT16_MAX};
    assert(room_diagnostics_pcm_level(full_scale, 2, 1) == 100);
    puts("room diagnostics metric tests passed");
    return 0;
}
