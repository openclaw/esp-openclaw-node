#include "room_diagnostics_metrics.h"

#include <limits.h>

static uint8_t perceptual_level(uint32_t magnitude)
{
    if (magnitude < 4) return 0;
    if (magnitude < 32) return (uint8_t)(1 + ((magnitude - 4) * 14U) / 28U);
    if (magnitude < 128) return (uint8_t)(15 + ((magnitude - 32) * 20U) / 96U);
    if (magnitude < 512) return (uint8_t)(35 + ((magnitude - 128) * 25U) / 384U);
    if (magnitude < 2048) return (uint8_t)(60 + ((magnitude - 512) * 25U) / 1536U);
    if (magnitude < 8192) return (uint8_t)(85 + ((magnitude - 2048) * 15U) / 6144U);
    return 100;
}

uint8_t room_diagnostics_pcm_level(
    const int16_t *samples,
    size_t sample_count,
    size_t channel_stride)
{
    if (samples == NULL || sample_count == 0 || channel_stride == 0) {
        return 0;
    }
    size_t channel_samples = (sample_count + channel_stride - 1U) / channel_stride;
    size_t sample_step = channel_stride;
    if (channel_samples > 128U) {
        sample_step *= (channel_samples + 127U) / 128U;
    }
    uint64_t sum = 0;
    uint32_t peak = 0;
    size_t taken = 0;
    for (size_t i = 0; i < sample_count; i += sample_step) {
        int32_t sample = samples[i];
        uint32_t magnitude = (uint32_t)(sample < 0 ? -sample : sample);
        sum += magnitude;
        if (magnitude > peak) peak = magnitude;
        ++taken;
    }
    uint32_t mean = taken > 0 ? (uint32_t)(sum / taken) : 0;
    uint8_t level = perceptual_level(mean);
    if (peak >= 16000) return 100;
    if (peak >= 8000 && level < 90) return 90;
    return level;
}
