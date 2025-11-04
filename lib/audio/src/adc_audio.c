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
static int16_t mono_samples[ADC_READ_LEN / 4];

// DC bias removal (12-bit ADC mid-point)
#define ADC_MID_CODE           2048

// DC blocking filter (1st order high-pass, ~20 Hz cutoff at 48 kHz)
#define DC_BLOCK_ALPHA         0.9974f  // exp(-2*pi*fc/fs)
static int16_t dc_block_prev_x = 0;
static int16_t dc_block_prev_y = 0;

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

    // Update to 48 kHz mono for v0.1 spec compliance
    adc_continuous_config_t dig_cfg = {
        .pattern_num = 1,  // Mono input
        .adc_pattern = adc_pattern,
        .sample_freq_hz = 48000,  // 48 kHz mono
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

    ESP_LOGI(TAG, "ADC continuous mode initialized (mono, 48 kHz, 24-bit)");
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

    // Parse ADC data (mono channel)
    size_t mono_count = 0;

    for (uint32_t i = 0; i < bytes_read; i += SOC_ADC_DIGI_RESULT_BYTES) {
        adc_digi_output_data_t *p = (adc_digi_output_data_t *)&adc_raw_data[i];

    // Check if valid data (TYPE2 format for ESP32-S3)
    uint32_t chan_num = p->type2.channel;
    uint32_t data = p->type2.data;

        // Only process the configured channel (left)
        if (chan_num == ADC_LEFT_CHANNEL) {
            // Remove DC bias and scale 12-bit to 16-bit (left-shift by 4)
            int16_t sample = (int16_t)((data - ADC_MID_CODE) << 4);

            // Apply DC blocking filter: y[n] = alpha * (x[n] - x[n-1]) + alpha * y[n-1]
            int16_t filtered_sample = (int16_t)(DC_BLOCK_ALPHA * (sample - dc_block_prev_x) +
                                                DC_BLOCK_ALPHA * dc_block_prev_y);
            dc_block_prev_x = sample;
            dc_block_prev_y = filtered_sample;

            mono_samples[mono_count++] = filtered_sample;
        }
    }

    // Duplicate mono to stereo (no upsampling needed, direct 48 kHz)
    size_t out_samples = mono_count;
    if (out_samples > num_samples) out_samples = num_samples;

    for (size_t i = 0; i < out_samples; i++) {
        stereo_buffer[i * 2] = mono_samples[i];     // Left
        stereo_buffer[i * 2 + 1] = mono_samples[i]; // Right
    }

    *samples_read = out_samples;

    if (mono_count > 0) {
    ESP_LOGD(TAG, "Read %zu mono samples (duplicated to stereo) from %lu bytes",
    mono_count, bytes_read);
    }

    return ESP_OK;
}
