#include "audio/i2s_audio.h"
#include "config/pins.h"
#include "config/build.h"
#include <driver/i2s_std.h>
#include <esp_log.h>
#include <string.h>

static const char *TAG = "i2s_audio";
static i2s_chan_handle_t tx_handle = NULL;
static i2s_chan_handle_t rx_handle = NULL;
static bool i2s_ready = false;

esp_err_t i2s_audio_init(void) {
    if (i2s_ready) return ESP_OK;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    esp_err_t ret = i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle);
    if (ret != ESP_OK) return ret;

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_IO, .bclk = I2S_BCK_IO, .ws = I2S_WS_IO,
            .dout = I2S_DO_IO, .din = GPIO_NUM_2,
        },
    };

    ret = i2s_channel_init_std_mode(tx_handle, &std_cfg);
    if (ret != ESP_OK) return ret;
    ret = i2s_channel_init_std_mode(rx_handle, &std_cfg);
    if (ret != ESP_OK) return ret;

    ret = i2s_channel_enable(tx_handle);
    if (ret != ESP_OK) return ret;
    ret = i2s_channel_enable(rx_handle);
    if (ret != ESP_OK) return ret;

    i2s_ready = true;
    ESP_LOGI(TAG, "I2S initialized (16-bit Philips)");
    return ESP_OK;
}

esp_err_t i2s_audio_read_stereo(int16_t *buffer, size_t max_frames, size_t *read) {
    if (!i2s_ready) return ESP_ERR_INVALID_STATE;
    size_t bytes_read = 0;
    esp_err_t ret = i2s_channel_read(rx_handle, buffer, max_frames * 4, &bytes_read, pdMS_TO_TICKS(100));
    *read = bytes_read / 4;
    return ret;
}

esp_err_t i2s_audio_write_stereo(const int16_t *buffer, size_t frames) {
    if (!i2s_ready) return ESP_ERR_INVALID_STATE;
    size_t bytes_written = 0;
    return i2s_channel_write(tx_handle, buffer, frames * 4, &bytes_written, pdMS_TO_TICKS(100));
}

esp_err_t i2s_audio_write_mono_as_stereo(const int16_t *buffer, size_t frames) {
    static int16_t temp[AUDIO_FRAME_SAMPLES * 2];
    for (size_t i = 0; i < frames; i++) {
        temp[i*2] = buffer[i];
        temp[i*2+1] = buffer[i];
    }
    return i2s_audio_write_stereo(temp, frames);
}

bool i2s_audio_is_ready(void) { return i2s_ready; }

esp_err_t i2s_audio_deinit(void) {
    if (!i2s_ready) return ESP_OK;
    if (rx_handle) { i2s_channel_disable(rx_handle); i2s_del_channel(rx_handle); rx_handle = NULL; }
    if (tx_handle) { i2s_channel_disable(tx_handle); i2s_del_channel(tx_handle); tx_handle = NULL; }
    i2s_ready = false;
    return ESP_OK;
}
