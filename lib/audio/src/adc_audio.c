#include "audio/adc_audio.h"

#include <esp_log.h>
#include <esp_adc/adc_continuous.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include "config/pins.h"
#include "config/build.h"

static const char *TAG = "adc_audio";

// DMA buffer size - enough for multiple frames
#define ADC_READ_LEN           1024
#define ADC_CONV_FRAME_SIZE    1024

// ADC continuous handle
static adc_continuous_handle_t adc_handle = NULL;

// Static buffers to avoid stack overflow
static int16_t left_samples[ADC_READ_LEN / 4];
static int16_t right_samples[ADC_READ_LEN / 4];

// DC bias removal (12-bit ADC mid-point)
#define ADC_MID_CODE           2048

esp_err_t adc_audio_init(void) {
    if (adc_handle != NULL) {
        ESP_LOGW(TAG, "ADC already initialized");
        return ESP_OK;
    }

    // Configure ADC continuous mode for stereo capture
    adc_continuous_handle_cfg_t adc_config = {
        .max_store_buf_size = ADC_CONV_FRAME_SIZE * 2,
        .conv_frame_size = ADC_CONV_FRAME_SIZE,
    };

    esp_err_t ret = adc_continuous_new_handle(&adc_config, &adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ADC handle: %s", esp_err_to_name(ret));
        return ret;
    }

    // Configure two-channel pattern (L and R interleaved)
    adc_digi_pattern_config_t adc_pattern[2] = {
        {
            .atten = ADC_ATTEN_DB_12,
            .channel = ADC_LEFT_CHANNEL,
            .unit = ADC_UNIT_1,
            .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH,
        },
        {
            .atten = ADC_ATTEN_DB_12,
            .channel = ADC_RIGHT_CHANNEL,
            .unit = ADC_UNIT_1,
            .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH,
        }
    };

    // ESP32-S3 ADC max is ~83 kHz total, but we can't hit exactly 96 kHz (48*2)
    // Use maximum safe rate for best quality
    adc_continuous_config_t dig_cfg = {
        .pattern_num = 2,
        .adc_pattern = adc_pattern,
        .sample_freq_hz = 83000,  // 41.5 kHz per channel (best we can do, will upsample slightly)
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE2,
    };

    ret = adc_continuous_config(adc_handle, &dig_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to config ADC: %s", esp_err_to_name(ret));
        adc_continuous_deinit(adc_handle);
        adc_handle = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "ADC continuous mode initialized (stereo, 41.5 kHz per channel, upsampling to 192 kHz)");
    return ESP_OK;
}

esp_err_t adc_audio_deinit(void) {
    if (adc_handle == NULL) {
        return ESP_OK;
    }

    adc_audio_stop();
    esp_err_t ret = adc_continuous_deinit(adc_handle);
    adc_handle = NULL;
    
    ESP_LOGI(TAG, "ADC deinitialized");
    return ret;
}

esp_err_t adc_audio_start(void) {
    if (adc_handle == NULL) {
        ESP_LOGE(TAG, "ADC not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = adc_continuous_start(adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start ADC: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "ADC started");
    return ESP_OK;
}

esp_err_t adc_audio_stop(void) {
    if (adc_handle == NULL) {
        return ESP_OK;
    }

    esp_err_t ret = adc_continuous_stop(adc_handle);
    ESP_LOGI(TAG, "ADC stopped");
    return ret;
}

esp_err_t adc_audio_read_stereo(int16_t *stereo_buffer, size_t num_samples, size_t *samples_read) {
    if (adc_handle == NULL) {
        ESP_LOGE(TAG, "ADC not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!stereo_buffer || !samples_read) {
        return ESP_ERR_INVALID_ARG;
    }

    *samples_read = 0;

    // Read raw ADC data
    uint8_t adc_raw_data[ADC_READ_LEN];
    uint32_t bytes_read = 0;
    
    esp_err_t ret = adc_continuous_read(adc_handle, adc_raw_data, ADC_READ_LEN, &bytes_read, 0);
    
    if (ret == ESP_ERR_TIMEOUT) {
        // No data available yet
        return ESP_OK;
    }
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC read failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Parse ADC data and demultiplex channels
    size_t left_count = 0;
    size_t right_count = 0;

    for (uint32_t i = 0; i < bytes_read; i += SOC_ADC_DIGI_RESULT_BYTES) {
        adc_digi_output_data_t *p = (adc_digi_output_data_t *)&adc_raw_data[i];
        
        // Check if valid data (TYPE2 format for ESP32-S3)
        uint32_t chan_num = p->type2.channel;
        uint32_t data = p->type2.data;

        // Remove DC bias and scale 12-bit to 16-bit
        int16_t sample = (int16_t)((data - ADC_MID_CODE) << 4);

        if (chan_num == ADC_LEFT_CHANNEL) {
            left_samples[left_count++] = sample;
        } else if (chan_num == ADC_RIGHT_CHANNEL) {
            right_samples[right_count++] = sample;
        }
    }

    // Interleave L/R and upsample from 41.5 kHz to 192 kHz (ratio ~4.63)
    size_t min_count = (left_count < right_count) ? left_count : right_count;
    
    if (min_count == 0) {
        *samples_read = 0;
        return ESP_OK;
    }
    
    // Linear interpolation upsampling
    const float ratio = 192000.0f / 41500.0f;  // ~4.63
    size_t out_samples = 0;
    
    for (out_samples = 0; out_samples < num_samples; out_samples++) {
        float src_pos = (float)out_samples / ratio;
        size_t src_idx = (size_t)src_pos;
        
        if (src_idx >= min_count - 1) break;
        
        float frac = src_pos - (float)src_idx;
        
        // Linear interpolation for both channels
        int32_t left_interp = (int32_t)(left_samples[src_idx] * (1.0f - frac) + 
                                         left_samples[src_idx + 1] * frac);
        int32_t right_interp = (int32_t)(right_samples[src_idx] * (1.0f - frac) + 
                                          right_samples[src_idx + 1] * frac);
        
        stereo_buffer[out_samples * 2] = (int16_t)left_interp;
        stereo_buffer[out_samples * 2 + 1] = (int16_t)right_interp;
    }

    *samples_read = out_samples;

    if (min_count > 0) {
        ESP_LOGD(TAG, "Read %zu stereo samples (L=%zu, R=%zu from %lu bytes)", 
                 min_count, left_count, right_count, bytes_read);
    }

    return ESP_OK;
}
