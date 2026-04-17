#include <unity.h>
#include "audio/pcm_convert.h"
#include <string.h>

void test_pcm_convert_stereo_to_mono_s16(void)
{
    const int16_t src_stereo[] = {1000, 2000, -500, -1500, 32767, -32768};
    int16_t dst_mono[3];
    
    pcm_convert_stereo_to_mono_s16(src_stereo, dst_mono, 3);
    
    TEST_ASSERT_EQUAL_INT16(1500, dst_mono[0]);   // (1000+2000)/2
    TEST_ASSERT_EQUAL_INT16(-1000, dst_mono[1]);  // (-500-1500)/2
    TEST_ASSERT_EQUAL_INT16(0, dst_mono[2]);      // (32767-32768)/2 = -0.5 -> 0 (int div)
}

void test_pcm_convert_mono_to_stereo_s16(void)
{
    const int16_t src_mono[] = {1234, -5678, 32767};
    int16_t dst_stereo[6];
    
    pcm_convert_mono_to_stereo_s16(src_mono, dst_stereo, 3);
    
    TEST_ASSERT_EQUAL_INT16(1234, dst_stereo[0]);
    TEST_ASSERT_EQUAL_INT16(1234, dst_stereo[1]);
    TEST_ASSERT_EQUAL_INT16(-5678, dst_stereo[2]);
    TEST_ASSERT_EQUAL_INT16(-5678, dst_stereo[3]);
}

void test_pcm_scale_s16(void)
{
    TEST_ASSERT_EQUAL_INT16(1000, pcm_scale_s16(1000, 1.0f));
    TEST_ASSERT_EQUAL_INT16(2000, pcm_scale_s16(1000, 2.0f));
    TEST_ASSERT_EQUAL_INT16(500, pcm_scale_s16(1000, 0.5f));
    TEST_ASSERT_EQUAL_INT16(32767, pcm_scale_s16(20000, 2.0f));  // Clamp
    TEST_ASSERT_EQUAL_INT16(-32768, pcm_scale_s16(-20000, 2.0f)); // Clamp
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_pcm_convert_stereo_to_mono_s16);
    RUN_TEST(test_pcm_convert_mono_to_stereo_s16);
    RUN_TEST(test_pcm_scale_s16);
    return UNITY_END();
}
