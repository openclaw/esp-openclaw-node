#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "room_pcm_gain.h"

static void test_pcm_range(void)
{
    static int16_t original[UINT16_MAX + 1U];
    static int16_t samples[UINT16_MAX + 1U];
    for (size_t i = 0; i <= UINT16_MAX; ++i) original[i] = (int16_t)(INT16_MIN + (int)i);
    const float gains_db[] = {0.0f, 0.25f, 3.0f, 6.0f, 12.0f};
    for (size_t g = 0; g < sizeof(gains_db) / sizeof(gains_db[0]); ++g) {
        memcpy(samples, original, sizeof(samples));
        uint32_t gain = room_pcm_gain_q12_from_db(gains_db[g]);
        room_pcm_gain_apply_s16(samples, UINT16_MAX + 1U, gain);
        if (gains_db[g] == 0.0f) assert(memcmp(samples, original, sizeof(samples)) == 0);
        for (size_t i = 0; i <= UINT16_MAX; ++i) {
            double ideal = round(original[i] * pow(10.0, gains_db[g] / 20.0));
            if (ideal > INT16_MAX) ideal = INT16_MAX;
            if (ideal < INT16_MIN) ideal = INT16_MIN;
            /* Q12 conversion contributes at most four PCM counts of error. */
            assert(fabs(samples[i] - ideal) <= 4.0);
            if (i > 0) assert(samples[i] >= samples[i - 1]);
        }
        assert(samples[0] == INT16_MIN && samples[UINT16_MAX] == INT16_MAX);
    }
}

static void test_rounding_and_channels(void)
{
    int16_t stereo[] = {100, -200, 300, -400};
    room_pcm_gain_apply_s16(stereo, 4, room_pcm_gain_q12_from_db(6.0f));
    assert(stereo[0] == 200 && stereo[1] == -399);
    assert(stereo[2] == 599 && stereo[3] == -798);

    int16_t halves[] = {1, -1, 3, -3, 0};
    room_pcm_gain_apply_s16(halves, 5, ROOM_PCM_GAIN_UNITY_Q12 / 2U);
    assert(halves[0] == 1 && halves[1] == -1);
    assert(halves[2] == 2 && halves[3] == -2 && halves[4] == 0);
    room_pcm_gain_apply_s16(NULL, 0, ROOM_PCM_GAIN_UNITY_Q12);
    room_pcm_gain_apply_s16(stereo, 0, room_pcm_gain_q12_from_db(12.0f));
    assert(stereo[0] == 200 && stereo[3] == -798);
}

static void test_tone_headroom(void)
{
    /* The diagnostic tone's -3 dBFS peak must survive inverse gain followed
     * by playback gain without turning into a clipped square wave. */
    const int16_t wave[] = {0, 8867, 16384, 21406, 23170, -8867, -16384, -21406, -23170};
    for (int quarter_db = 0; quarter_db <= 48; ++quarter_db) {
        uint32_t gain = room_pcm_gain_q12_from_db(quarter_db / 4.0f);
        uint32_t inverse = (ROOM_PCM_GAIN_UNITY_Q12 * ROOM_PCM_GAIN_UNITY_Q12 + gain / 2U) / gain;
        int16_t samples[sizeof(wave) / sizeof(wave[0])];
        memcpy(samples, wave, sizeof(wave));
        room_pcm_gain_apply_s16(samples, sizeof(samples) / sizeof(samples[0]), inverse);
        room_pcm_gain_apply_s16(samples, sizeof(samples) / sizeof(samples[0]), gain);
        for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
            assert(abs(samples[i] - wave[i]) <= 12);
            assert(samples[i] > INT16_MIN && samples[i] < INT16_MAX);
        }
    }
}

int main(void)
{
    test_pcm_range();
    test_rounding_and_channels();
    test_tone_headroom();
    puts("room PCM gain tests passed");
    return 0;
}
