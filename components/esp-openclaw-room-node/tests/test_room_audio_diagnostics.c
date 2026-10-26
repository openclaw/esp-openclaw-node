#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "room_diagnostics_data.h"

static int64_t now_us;
static int feed_result;

int64_t esp_timer_get_time(void)
{
    return now_us;
}

static int fake_feed(void *data, const int16_t *pcm)
{
    assert(data != NULL && pcm != NULL);
    return feed_result;
}

typedef struct {
    int (*feed)(void *, const int16_t *);
} afe_interface_t;

typedef struct {
    const afe_interface_t *afe_handle;
    void *afe_data;
} audio_aec_src_t;

#include "afe_feed_boundary.h"

int main(void)
{
    const afe_interface_t iface = {.feed = fake_feed};
    int16_t pcm[2] = {0};
    audio_aec_src_t src = {.afe_handle = &iface, .afe_data = pcm};
    const int results[] = {640, 0, -1};
    for (size_t i = 0; i < sizeof(results) / sizeof(results[0]); ++i) {
        feed_result = results[i];
        assert(audio_aec_feed_data(&src, (uint8_t *)pcm, sizeof(pcm)) == feed_result);
    }
    room_audio_diagnostics_snapshot_t snapshot;
    room_diagnostics_audio_get(&snapshot);
    assert(snapshot.feed_successes == 1);
    assert(snapshot.feed_errors == 2);

    room_diagnostics_audio_set_capture_mode(ROOM_DIAGNOSTICS_AFE_TALK_VC, false, 640, 640);
    now_us = 1000;
    room_diagnostics_audio_record_read_entry();
    room_diagnostics_audio_get(&snapshot);
    assert(snapshot.max_read_interval_us == 0);
    now_us += 32000;
    room_diagnostics_audio_record_read_entry();
    now_us += 200000;
    room_diagnostics_audio_record_read_entry();
    now_us += 10000;
    room_diagnostics_audio_record_read_entry();
    room_diagnostics_audio_record_fetch(pcm, 2, true, true, 25, 12000);
    room_diagnostics_audio_record_fetch(pcm, 2, true, true, 80, 1000);
    room_diagnostics_audio_get(&snapshot);
    assert(snapshot.max_read_interval_us == 200000);
    assert(snapshot.max_fetch_duration_us == 12000);
    assert(snapshot.ringbuffer_free_percent == 80);

    /* Restarting AFE must not count the gap between runs as backpressure. */
    room_diagnostics_audio_set_capture_mode(ROOM_DIAGNOSTICS_AFE_AMBIENT_SR, true, 640, 640);
    now_us += 9000000;
    room_diagnostics_audio_record_read_entry();
    room_diagnostics_audio_get(&snapshot);
    assert(snapshot.max_read_interval_us == 0);
    assert(snapshot.max_fetch_duration_us == 0);
    assert(snapshot.feed_successes == 1 && snapshot.feed_errors == 2);
    puts("room audio diagnostics tests passed");
    return 0;
}
