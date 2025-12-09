#include "audio/i2s_audio.h"
#include "config/pins.h"
#include "config/build.h"
#include <driver/i2s_std.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>

static const char *TAG = "i2s_audio";
static i2s_chan_handle_t tx_handle = NULL;

esp_err_t i2s_audio_init(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    // Use default DMA config (6 desc × 240 frames = ~5.7KB) to save internal SRAM
    // Custom DMA config (8 × 480 = ~15KB) causes task creation to fail due to OOM
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = AUDIO_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,  // UDA1334 has internal PLL; MCLK helps ESP32 timing
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,  // UDA1334 has built-in PLL - leave MCLK unconnected (not GPIO1)
            .bclk = I2S_BCK_IO,
            .ws = I2S_WS_IO,
            .dout = I2S_DO_IO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
    
    ESP_LOGI(TAG, "I2S initialized: %dHz, 16-bit, stereo (UDA1334 with internal PLL)", AUDIO_SAMPLE_RATE);
    return ESP_OK;
}

esp_err_t i2s_audio_write_samples(const int16_t *samples, size_t num_samples) {
    if (!tx_handle) return ESP_ERR_INVALID_STATE;
    
    size_t bytes_written;
    esp_err_t ret = i2s_channel_write(tx_handle, samples,
    num_samples * sizeof(int16_t),
    &bytes_written, portMAX_DELAY);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S write failed");
        return ret;
    }
    
    return ESP_OK;
}

esp_err_t i2s_audio_write_mono_as_stereo(const int16_t *mono_samples, size_t num_mono_samples) {
    if (!tx_handle) return ESP_ERR_INVALID_STATE;

    static int16_t stereo_buffer[AUDIO_FRAME_SAMPLES * 2];
    static uint32_t write_count = 0;
    
    for (size_t i = 0; i < num_mono_samples; i++) {
        stereo_buffer[i * 2] = mono_samples[i];
        stereo_buffer[i * 2 + 1] = mono_samples[i];
    }
    
    size_t bytes_written;
    esp_err_t ret = i2s_channel_write(tx_handle, stereo_buffer,
    num_mono_samples * 2 * sizeof(int16_t),
    &bytes_written, portMAX_DELAY);
    
    write_count++;
    if ((write_count % 100) == 1) {
        ESP_LOGI(TAG, "I2S write #%lu: ret=%d, bytes_written=%zu, samples[0]=%d samples[1]=%d", 
                 write_count, ret, bytes_written, (int)stereo_buffer[0], (int)stereo_buffer[1]);
    }
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S stereo write failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}
