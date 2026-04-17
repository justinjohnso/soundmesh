#include "audio/adc_audio.h"
#include "audio/pcm_convert.h"
#include <esp_log.h>
#include <esp_adc/adc_continuous.h>
#include <string.h>

static adc_continuous_handle_t adc_handle = NULL;
static bool adc_ready = false;

esp_err_t adc_audio_init(void) {
    if (adc_ready) return ESP_OK;
    
    adc_continuous_handle_cfg_t adc_config = {
        .max_store_buf_size = 1024,
        .conv_frame_size = 256,
    };
    esp_err_t ret = adc_continuous_new_handle(&adc_config, &adc_handle);
    if (ret != ESP_OK) return ret;

    adc_continuous_config_t config = {
        .sample_freq_hz = 48000,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE1,
    };
    
    adc_digi_pattern_config_t adc_pattern[2] = {
        {.atten = ADC_ATTEN_DB_12, .channel = 0, .unit = ADC_UNIT_1, .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH},
        {.atten = ADC_ATTEN_DB_12, .channel = 1, .unit = ADC_UNIT_1, .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH}
    };
    config.pattern_num = 2;
    config.adc_pattern = adc_pattern;

    ret = adc_continuous_config(adc_handle, &config);
    if (ret != ESP_OK) return ret;

    ret = adc_continuous_start(adc_handle);
    if (ret == ESP_OK) adc_ready = true;
    return ret;
}

esp_err_t adc_audio_read_stereo(int16_t *buffer, size_t max_frames, size_t *read) {
    if (!adc_ready) return ESP_ERR_INVALID_STATE;
    uint8_t raw[1024];
    uint32_t out_len = 0;
    esp_err_t ret = adc_continuous_read(adc_handle, raw, sizeof(raw), &out_len, 10);
    if (ret != ESP_OK) return ret;

    size_t count = out_len / 4; // 2 channels * 2 bytes
    if (count > max_frames) count = max_frames;

    for (size_t i = 0; i < count; i++) {
        int16_t l = (((int16_t)((raw[i*4+1] << 8) | raw[i*4])) & 0x0FFF) - 2048;
        int16_t r = (((int16_t)((raw[i*4+3] << 8) | raw[i*4+2])) & 0x0FFF) - 2048;
        buffer[i*2] = l << 4;
        buffer[i*2+1] = r << 4;
    }
    *read = count;
    return ESP_OK;
}

esp_err_t adc_audio_deinit(void) {
    if (adc_handle) {
        adc_continuous_stop(adc_handle);
        adc_continuous_deinit(adc_handle);
        adc_handle = NULL;
    }
    adc_ready = false;
    return ESP_OK;
}
bool adc_audio_is_ready(void) { return adc_ready; }
