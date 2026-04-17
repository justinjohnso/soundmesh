/**
 * ES8388 Audio Codec Driver
 * 
 * Optimized for pure 16-bit Philips I2S standard.
 * Prevents I2C EMI by performing all power-up configuration BEFORE starting I2S MCLK.
 */

#include "audio/es8388_audio.h"
#include "config/pins.h"
#include "config/build.h"

#include <driver/i2c.h>
#include <driver/i2s_std.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

static const char *TAG = "es8388_audio";

// ES8388 I2C address
#define ES8388_ADDR 0x10

// ES8388 Register Addresses
#define ES8388_CONTROL1         0x00
#define ES8388_CONTROL2         0x01
#define ES8388_CHIPPOWER        0x02
#define ES8388_ADCPOWER         0x03
#define ES8388_DACPOWER         0x04
#define ES8388_MASTERMODE       0x08
#define ES8388_ADCCONTROL1      0x09
#define ES8388_ADCCONTROL2      0x0A
#define ES8388_ADCCONTROL3      0x0B
#define ES8388_ADCCONTROL4      0x0C
#define ES8388_ADCCONTROL5      0x0D
#define ES8388_ADCCONTROL8      0x10
#define ES8388_ADCCONTROL9      0x11
#define ES8388_DACCONTROL1      0x17
#define ES8388_DACCONTROL2      0x18
#define ES8388_DACCONTROL3      0x19
#define ES8388_DACCONTROL4      0x1A
#define ES8388_DACCONTROL5      0x1B
#define ES8388_DACCONTROL16     0x26
#define ES8388_DACCONTROL17     0x27
#define ES8388_DACCONTROL20     0x2A
#define ES8388_DACCONTROL21     0x2B
#define ES8388_DACCONTROL23     0x2D
#define ES8388_DACCONTROL24     0x2E
#define ES8388_DACCONTROL25     0x2F
#define ES8388_DACCONTROL26     0x30
#define ES8388_DACCONTROL27     0x31

#define ADC_INPUT_LINPUT2_RINPUT2   0x50
#define DAC_OUTPUT_ALL              0x3C

static i2s_chan_handle_t i2s_tx_handle = NULL;
static i2s_chan_handle_t i2s_rx_handle = NULL;
static bool es8388_initialized = false;
static bool dac_enabled = false;

static esp_err_t es8388_write_reg(uint8_t reg, uint8_t val) {
    uint8_t data[2] = {reg, val};
    esp_err_t ret = i2c_master_write_to_device(I2C_MASTER_NUM, ES8388_ADDR, data, 2, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C write failed: reg=0x%02x, err=%s", reg, esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t es8388_read_reg(uint8_t reg, uint8_t *val) {
    return i2c_master_write_read_device(I2C_MASTER_NUM, ES8388_ADDR, &reg, 1, val, 1, pdMS_TO_TICKS(100));
}

static esp_err_t es8388_codec_init(bool enable_dac) {
    esp_err_t res = ESP_OK;
    ESP_LOGI(TAG, "Initializing ES8388 codec (DAC=%s)", enable_dac ? "enabled" : "disabled");
    
    res |= es8388_write_reg(ES8388_DACCONTROL3, 0x04);  // Mute DAC
    res |= es8388_write_reg(ES8388_CONTROL2, 0x50);     // Low power refs
    res |= es8388_write_reg(ES8388_CHIPPOWER, 0x00);    // Power up all
    res |= es8388_write_reg(ES8388_MASTERMODE, 0x00);   // Slave mode
    
    // Configure DAC (16-bit Philips I2S)
    res |= es8388_write_reg(ES8388_DACPOWER, 0xC0);     // Power down outputs initially
    res |= es8388_write_reg(ES8388_CONTROL1, 0x12);     // ADC+DAC mode
    res |= es8388_write_reg(ES8388_DACCONTROL1, 0x18);  // 16-bit I2S
    res |= es8388_write_reg(ES8388_DACCONTROL2, 0x02);  // 256*Fs ratio
    res |= es8388_write_reg(ES8388_DACCONTROL16, 0x00); // I2S input
    res |= es8388_write_reg(ES8388_DACCONTROL17, 0x90); // Left DAC to Mixer
    res |= es8388_write_reg(ES8388_DACCONTROL20, 0x90); // Right DAC to Mixer
    res |= es8388_write_reg(ES8388_DACCONTROL21, 0x80); // Share LRCK
    
    // Configure ADC (16-bit Philips I2S)
    res |= es8388_write_reg(ES8388_ADCCONTROL1, 0x00);  // 0dB PGA gain
    res |= es8388_write_reg(ES8388_ADCCONTROL2, ADC_INPUT_LINPUT2_RINPUT2);
    res |= es8388_write_reg(ES8388_ADCCONTROL3, 0x02);  // Stereo
    res |= es8388_write_reg(ES8388_ADCCONTROL4, 0x0C);  // 16-bit I2S
    res |= es8388_write_reg(ES8388_ADCCONTROL5, 0x02);  // 256*Fs ratio
    res |= es8388_write_reg(ES8388_ADCCONTROL8, 0x00);  // Left ADC volume 0dB
    res |= es8388_write_reg(ES8388_ADCCONTROL9, 0x00);  // Right ADC volume 0dB
    
    // Final Power Up (BEFORE I2S/MCLK to avoid EMI)
    res |= es8388_write_reg(ES8388_ADCPOWER, 0x00);     // ADC On
    
    if (enable_dac) {
        // 0x00 is unity / max DAC output level for ES8388 volume registers.
        // 0x1E was effectively a heavy attenuation and made monitor/OUT playback near-silent.
        res |= es8388_write_reg(ES8388_DACCONTROL24, 0x00);
        res |= es8388_write_reg(ES8388_DACCONTROL25, 0x00);
        res |= es8388_write_reg(ES8388_DACPOWER, DAC_OUTPUT_ALL);
        res |= es8388_write_reg(ES8388_DACCONTROL3, 0x00);   // Unmute
        dac_enabled = true;
    }
    
    return res;
}

static esp_err_t i2s_init(bool enable_dac) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    
    esp_err_t ret = i2s_new_channel(&chan_cfg, enable_dac ? &i2s_tx_handle : NULL, &i2s_rx_handle);
    if (ret != ESP_OK) return ret;

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = AUDIO_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = ES8388_MCLK_IO, .bclk = ES8388_BCLK_IO, .ws = ES8388_WS_IO,
            .dout = ES8388_DOUT_IO, .din = ES8388_DIN_IO,
        },
    };

    ret = i2s_channel_init_std_mode(i2s_rx_handle, &std_cfg);
    if (ret != ESP_OK) return ret;
    if (enable_dac) {
        ret = i2s_channel_init_std_mode(i2s_tx_handle, &std_cfg);
        if (ret != ESP_OK) return ret;
    }

    ret = i2s_channel_enable(i2s_rx_handle);
    if (ret != ESP_OK) return ret;
    if (enable_dac) {
        ret = i2s_channel_enable(i2s_tx_handle);
    }
    return ret;
}

esp_err_t es8388_audio_init(bool enable_dac) {
    if (es8388_initialized) return ESP_OK;
    
    uint8_t test_val;
    esp_err_t codec_ret = es8388_read_reg(ES8388_CONTROL1, &test_val);
    if (codec_ret == ESP_OK) {
        if (es8388_codec_init(enable_dac) != ESP_OK) {
            ESP_LOGW(TAG, "ES8388 codec config failed, continuing with raw I2S");
        }
    } else {
        ESP_LOGW(TAG, "ES8388 codec not detected (err=%s), continuing with raw I2S", esp_err_to_name(codec_ret));
    }

    // Always attempt to init I2S so handles are valid for adf_pipeline tasks
    esp_err_t i2s_ret = i2s_init(enable_dac);
    if (i2s_ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S hardware init failed: %s", esp_err_to_name(i2s_ret));
        return i2s_ret;
    }

    es8388_initialized = true;
    ESP_LOGI(TAG, "ES8388 audio driver initialized (16-bit Philips, codec_present=%d)", 
             (codec_ret == ESP_OK));
    return ESP_OK;
}

esp_err_t es8388_audio_deinit(void) {
    if (!es8388_initialized) return ESP_OK;
    if (i2s_rx_handle) { i2s_channel_disable(i2s_rx_handle); i2s_del_channel(i2s_rx_handle); i2s_rx_handle = NULL; }
    if (i2s_tx_handle) { i2s_channel_disable(i2s_tx_handle); i2s_del_channel(i2s_tx_handle); i2s_tx_handle = NULL; }
    es8388_initialized = false;
    return ESP_OK;
}

esp_err_t es8388_audio_read_stereo(int16_t *buffer, size_t max_frames, size_t *frames_read) {
    size_t bytes_read = 0;
    esp_err_t ret = i2s_channel_read(i2s_rx_handle, buffer, max_frames * 4, &bytes_read, pdMS_TO_TICKS(100));
    *frames_read = bytes_read / 4;
    return ret;
}

esp_err_t es8388_audio_write_stereo(const int16_t *buffer, size_t frames) {
    size_t bytes_written = 0;
    return i2s_channel_write(i2s_tx_handle, buffer, frames * 4, &bytes_written, pdMS_TO_TICKS(100));
}

esp_err_t es8388_audio_set_volume(uint8_t volume) {
    uint8_t reg_val = (100 - volume) * 33 / 100;
    es8388_write_reg(ES8388_DACCONTROL24, reg_val);
    es8388_write_reg(ES8388_DACCONTROL25, reg_val);
    return ESP_OK;
}

esp_err_t es8388_audio_set_input_gain(uint8_t gain_db) {
    uint8_t gn = (gain_db / 3) & 0x0F;
    return es8388_write_reg(ES8388_ADCCONTROL1, (gn << 4) | gn);
}

bool es8388_audio_is_ready(void) { return es8388_initialized; }
