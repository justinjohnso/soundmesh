#include "audio/sink.h"
#include "common/config.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "i2s_dac_sink";

static i2s_chan_handle_t tx_handle = NULL;
static bool initialized = false;

static esp_err_t i2s_dac_init(const void *cfg) {
    (void)cfg;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2C_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCK_IO,
            .ws = I2S_WS_IO,
            .dout = I2S_DATA_OUT_IO,
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

    initialized = true;
    ESP_LOGI(TAG, "I2S DAC sink initialized (%d Hz, mono)", AUDIO_SAMPLE_RATE);
    return ESP_OK;
}

static size_t i2s_dac_write(const int16_t *src, size_t frames, uint32_t timeout_ms) {
    if (!initialized || !src) {
        return 0;
    }

    size_t bytes_written = 0;
    esp_err_t ret = i2s_channel_write(tx_handle, src, frames * sizeof(int16_t),
                                     &bytes_written, pdMS_TO_TICKS(timeout_ms));

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2S write failed: %d", ret);
        return 0;
    }

    return bytes_written / sizeof(int16_t);
}

static void i2s_dac_deinit(void) {
    if (tx_handle) {
        i2s_channel_disable(tx_handle);
        i2s_del_channel(tx_handle);
        tx_handle = NULL;
    }
    initialized = false;
    ESP_LOGI(TAG, "I2S DAC sink deinitialized");
}

const audio_sink_t i2s_dac_sink = {
    .init = i2s_dac_init,
    .write = i2s_dac_write,
    .deinit = i2s_dac_deinit,
};
