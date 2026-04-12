#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PCM_S24_MIN_IN_S32 (-8388608)
#define PCM_S24_MAX_IN_S32 (8388607)

static inline int32_t pcm_clamp_s24_in_s32(int32_t sample)
{
    if (sample > PCM_S24_MAX_IN_S32) {
        return PCM_S24_MAX_IN_S32;
    }
    if (sample < PCM_S24_MIN_IN_S32) {
        return PCM_S24_MIN_IN_S32;
    }
    return sample;
}

static inline int32_t pcm_s16_to_s24_in_s32(int16_t sample)
{
    return ((int32_t)sample) << 8;
}

static inline int16_t pcm_s24_in_s32_to_s16(int32_t sample)
{
    int32_t clamped = pcm_clamp_s24_in_s32(sample);
    int32_t rounded;
    if (clamped >= 0) {
        rounded = clamped + 0x80;
    } else {
        rounded = clamped - 0x80;
    }
    int32_t shifted = rounded >> 8;
    if (shifted > INT16_MAX) {
        return INT16_MAX;
    }
    if (shifted < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)shifted;
}

static inline void pcm_convert_mono_s16_to_s24(const int16_t *src, int32_t *dst, size_t sample_count)
{
    if (!src || !dst) {
        return;
    }
    for (size_t i = 0; i < sample_count; i++) {
        dst[i] = pcm_s16_to_s24_in_s32(src[i]);
    }
}

static inline void pcm_convert_mono_s24_to_s16(const int32_t *src, int16_t *dst, size_t sample_count)
{
    if (!src || !dst) {
        return;
    }
    for (size_t i = 0; i < sample_count; i++) {
        dst[i] = pcm_s24_in_s32_to_s16(src[i]);
    }
}

static inline void pcm_convert_stereo_s16_to_mono_s24(const int16_t *src_stereo,
                                                       int32_t *dst_mono,
                                                       size_t frame_count)
{
    if (!src_stereo || !dst_mono) {
        return;
    }
    for (size_t i = 0; i < frame_count; i++) {
        int32_t l = src_stereo[i * 2];
        int32_t r = src_stereo[i * 2 + 1];
        int32_t mono_s16 = (l + r) / 2;
        dst_mono[i] = mono_s16 << 8;
    }
}

static inline void pcm_convert_mono_s24_to_stereo_s16(const int32_t *src_mono,
                                                       int16_t *dst_stereo,
                                                       size_t frame_count)
{
    if (!src_mono || !dst_stereo) {
        return;
    }
    for (size_t i = 0; i < frame_count; i++) {
        int16_t sample = pcm_s24_in_s32_to_s16(src_mono[i]);
        dst_stereo[i * 2] = sample;
        dst_stereo[i * 2 + 1] = sample;
    }
}

#ifdef __cplusplus
}
#endif
