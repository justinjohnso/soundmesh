#include <unity.h>

#include "audio/pcm_convert.h"

void test_pcm_clamp_s24_in_s32_saturates_at_24_bit_bounds(void)
{
    TEST_ASSERT_EQUAL_INT32(PCM_S24_MAX_IN_S32, pcm_clamp_s24_in_s32(PCM_S24_MAX_IN_S32 + 1));
    TEST_ASSERT_EQUAL_INT32(PCM_S24_MIN_IN_S32, pcm_clamp_s24_in_s32(PCM_S24_MIN_IN_S32 - 1));
    TEST_ASSERT_EQUAL_INT32(123456, pcm_clamp_s24_in_s32(123456));
}

void test_pcm_s16_to_s24_in_s32_left_aligns_sign_and_magnitude(void)
{
    TEST_ASSERT_EQUAL_INT32(0, pcm_s16_to_s24_in_s32(0));
    TEST_ASSERT_EQUAL_INT32(256, pcm_s16_to_s24_in_s32(1));
    TEST_ASSERT_EQUAL_INT32(-256, pcm_s16_to_s24_in_s32(-1));
    TEST_ASSERT_EQUAL_INT32(8388352, pcm_s16_to_s24_in_s32(32767));
    TEST_ASSERT_EQUAL_INT32(-8388608, pcm_s16_to_s24_in_s32(-32768));
}

void test_pcm_s24_in_s32_to_s16_rounds_and_saturates_to_clamped_extrema(void)
{
    TEST_ASSERT_EQUAL_INT16(0, pcm_s24_in_s32_to_s16(127));
    TEST_ASSERT_EQUAL_INT16(1, pcm_s24_in_s32_to_s16(128));
    TEST_ASSERT_EQUAL_INT16(-1, pcm_s24_in_s32_to_s16(-128));
    TEST_ASSERT_EQUAL_INT16(-2, pcm_s24_in_s32_to_s16(-129));

    TEST_ASSERT_EQUAL_INT16(32767, pcm_s24_in_s32_to_s16(PCM_S24_MAX_IN_S32));
    TEST_ASSERT_EQUAL_INT16(-32768, pcm_s24_in_s32_to_s16(PCM_S24_MIN_IN_S32));
    TEST_ASSERT_EQUAL_INT16(32767, pcm_s24_in_s32_to_s16(PCM_S24_MAX_IN_S32 + 5000));
    TEST_ASSERT_EQUAL_INT16(-32768, pcm_s24_in_s32_to_s16(PCM_S24_MIN_IN_S32 - 5000));
}

void test_pcm_convert_mono_s16_to_s24_maps_each_sample(void)
{
    const int16_t src[] = {-32768, -1, 0, 1, 32767};
    int32_t dst[5] = {0};

    pcm_convert_mono_s16_to_s24(src, dst, 5);

    TEST_ASSERT_EQUAL_INT32(-8388608, dst[0]);
    TEST_ASSERT_EQUAL_INT32(-256, dst[1]);
    TEST_ASSERT_EQUAL_INT32(0, dst[2]);
    TEST_ASSERT_EQUAL_INT32(256, dst[3]);
    TEST_ASSERT_EQUAL_INT32(8388352, dst[4]);
}

void test_pcm_convert_mono_s24_to_s16_maps_each_sample(void)
{
    const int32_t src[] = {-129, -128, 127, 128, PCM_S24_MAX_IN_S32 + 42};
    int16_t dst[5] = {0};

    pcm_convert_mono_s24_to_s16(src, dst, 5);

    TEST_ASSERT_EQUAL_INT16(-2, dst[0]);
    TEST_ASSERT_EQUAL_INT16(-1, dst[1]);
    TEST_ASSERT_EQUAL_INT16(0, dst[2]);
    TEST_ASSERT_EQUAL_INT16(1, dst[3]);
    TEST_ASSERT_EQUAL_INT16(32767, dst[4]);
}

void test_pcm_convert_stereo_s16_to_mono_s24_averages_channels_before_widening(void)
{
    const int16_t src_stereo[] = {
        1000, 3000,
        -2000, -4000,
        32767, -32768
    };
    int32_t dst_mono[3] = {0};

    pcm_convert_stereo_s16_to_mono_s24(src_stereo, dst_mono, 3);

    TEST_ASSERT_EQUAL_INT32(512000, dst_mono[0]);
    TEST_ASSERT_EQUAL_INT32(-768000, dst_mono[1]);
    TEST_ASSERT_EQUAL_INT32(0, dst_mono[2]);
}

void test_pcm_convert_mono_s24_to_stereo_s16_duplicates_output_channels(void)
{
    const int32_t src_mono[] = {128, -129, PCM_S24_MIN_IN_S32 - 77};
    int16_t dst_stereo[6] = {0};

    pcm_convert_mono_s24_to_stereo_s16(src_mono, dst_stereo, 3);

    TEST_ASSERT_EQUAL_INT16(1, dst_stereo[0]);
    TEST_ASSERT_EQUAL_INT16(1, dst_stereo[1]);
    TEST_ASSERT_EQUAL_INT16(-2, dst_stereo[2]);
    TEST_ASSERT_EQUAL_INT16(-2, dst_stereo[3]);
    TEST_ASSERT_EQUAL_INT16(-32768, dst_stereo[4]);
    TEST_ASSERT_EQUAL_INT16(-32768, dst_stereo[5]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_pcm_clamp_s24_in_s32_saturates_at_24_bit_bounds);
    RUN_TEST(test_pcm_s16_to_s24_in_s32_left_aligns_sign_and_magnitude);
    RUN_TEST(test_pcm_s24_in_s32_to_s16_rounds_and_saturates_to_clamped_extrema);
    RUN_TEST(test_pcm_convert_mono_s16_to_s24_maps_each_sample);
    RUN_TEST(test_pcm_convert_mono_s24_to_s16_maps_each_sample);
    RUN_TEST(test_pcm_convert_stereo_s16_to_mono_s24_averages_channels_before_widening);
    RUN_TEST(test_pcm_convert_mono_s24_to_stereo_s16_duplicates_output_channels);
    return UNITY_END();
}
