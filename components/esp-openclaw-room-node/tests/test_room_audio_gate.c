#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "room_audio_gate.h"

static void play(room_audio_gate_t *gate)
{
    const int16_t speaker[] = {0, 2000, -2000, 0};
    assert(room_audio_gate_begin_playback(gate, speaker, 4));
}

static void capture_block(room_audio_gate_t *gate, unsigned duration_us, bool silent)
{
    int16_t pcm[] = {1234, -2345, 32767, -32768};
    const int16_t original[] = {1234, -2345, 32767, -32768};
    room_audio_gate_filter(gate, pcm, sizeof(pcm), duration_us);
    if (silent) {
        for (size_t i = 0; i < sizeof(pcm) / sizeof(pcm[0]); ++i) assert(pcm[i] == 0);
    } else {
        assert(memcmp(pcm, original, sizeof(pcm)) == 0);
    }
}

static void test_single_mic_turns(void)
{
    room_audio_gate_t gate;
    room_audio_gate_init(&gate, "M");
    capture_block(&gate, 20000, false);
    play(&gate);
    /* A slow/blocking driver must not exhaust the tail while still writing. */
    for (int i = 0; i < 100; ++i) capture_block(&gate, 20000, true);
    room_audio_gate_end_playback(&gate);
    for (int i = 0; i < 5; ++i) capture_block(&gate, 20000, true);
    /* A new speaker chunk refreshes the tail instead of accumulating delay. */
    play(&gate);
    room_audio_gate_end_playback(&gate);
    for (int i = 0; i < 15; ++i) capture_block(&gate, 20000, true);
    capture_block(&gate, 20000, false);
    /* A final capture block may straddle the end of the 300 ms tail. */
    play(&gate);
    room_audio_gate_end_playback(&gate);
    for (int i = 0; i < 5; ++i) capture_block(&gate, 64000, true);
    capture_block(&gate, 64000, false);
}

static void test_silent_playback(void)
{
    room_audio_gate_t gate;
    room_audio_gate_init(&gate, "M");
    const int16_t silence[] = {0, 64, -64, 0};
    for (int i = 0; i < 100; ++i) {
        assert(!room_audio_gate_begin_playback(&gate, silence, 4));
        capture_block(&gate, 20000, false);
    }
    play(&gate);
    room_audio_gate_end_playback(&gate);
    for (int i = 0; i < 15; ++i) {
        assert(!room_audio_gate_begin_playback(&gate, silence, 4));
        capture_block(&gate, 20000, true);
    }
    capture_block(&gate, 20000, false);
}

static void test_reference_capture(void)
{
    const char *layouts[] = {"MR", "MMNR", NULL};
    for (size_t i = 0; i < sizeof(layouts) / sizeof(layouts[0]); ++i) {
        room_audio_gate_t gate;
        room_audio_gate_init(&gate, layouts[i]);
        const int16_t speaker[] = {32767, -32768};
        assert(!room_audio_gate_begin_playback(&gate, speaker, 2));
        capture_block(&gate, 20000, false);
        room_audio_gate_end_playback(&gate);
        capture_block(&gate, 20000, false);
    }
    capture_block(NULL, 20000, false);
}

int main(void)
{
    test_single_mic_turns();
    test_silent_playback();
    test_reference_capture();
    puts("room audio gate tests passed");
    return 0;
}
