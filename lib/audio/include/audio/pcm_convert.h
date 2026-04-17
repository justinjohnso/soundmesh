#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Optimized 16-bit PCM utilities for ESP32-S3.
 * Eliminates 24-bit overhead to prevent CPU starvation.
 */

static inline void pcm_convert_stereo_to_mono_s16(const int16_t *src_stereo,
                                                 int16_t *dst_mono,
                                                 size_t frame_count)
{
    if (!src_stereo || !dst_mono) return;
    for (size_t i = 0; i < frame_count; i++) {
        // Simple average to mono
        dst_mono[i] = (int16_t)(((int32_t)src_stereo[i * 2] + (int32_t)src_stereo[i * 2 + 1]) / 2);
    }
}

static inline void pcm_convert_mono_to_stereo_s16(const int16_t *src_mono,
                                                 int16_t *dst_stereo,
                                                 size_t frame_count)
{
    if (!src_mono || !dst_stereo) return;
    for (size_t i = 0; i < frame_count; i++) {
        dst_stereo[i * 2] = src_mono[i];
        dst_stereo[i * 2 + 1] = src_mono[i];
    }
}

static inline int16_t pcm_scale_s16(int16_t sample, float gain)
{
    int32_t scaled = (int32_t)((float)sample * gain);
    if (scaled > INT16_MAX) return INT16_MAX;
    if (scaled < INT16_MIN) return INT16_MIN;
    return (int16_t)scaled;
}

#ifdef __cplusplus
}
#endif
