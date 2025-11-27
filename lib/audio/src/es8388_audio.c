/**
 * ES8388 Audio Codec Driver
 * 
 * Based on ESP-ADF ES8388 driver, adapted for MeshNet Audio project.
 * Uses ESP-IDF I2C and I2S drivers directly.
 * 
 * References:
 * - ESP-ADF: https://github.com/espressif/esp-adf/blob/master/components/audio_hal/driver/es8388/es8388.c
 * - PCBArtists ES8388 Module: https://pcbartists.com/products/es8388-module/
 * - thaaraak/ESP32-ES8388: https://github.com/thaaraak/ESP32-ES8388
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

// ES8388 I2C address (directly from module datasheet)
#define ES8388_ADDR 0x10

// ES8388 Register Addresses
#define ES8388_CONTROL1         0x00
#define ES8388_CONTROL2         0x01
#define ES8388_CHIPPOWER        0x02
#define ES8388_ADCPOWER         0x03
#define ES8388_DACPOWER         0x04
#define ES8388_CHIPLOPOW1       0x05
#define ES8388_CHIPLOPOW2       0x06
#define ES8388_ANAVOLMANAG      0x07
#define ES8388_MASTERMODE       0x08

#define ES8388_ADCCONTROL1      0x09
#define ES8388_ADCCONTROL2      0x0A
#define ES8388_ADCCONTROL3      0x0B
#define ES8388_ADCCONTROL4      0x0C
#define ES8388_ADCCONTROL5      0x0D
#define ES8388_ADCCONTROL6      0x0E
#define ES8388_ADCCONTROL7      0x0F
#define ES8388_ADCCONTROL8      0x10
#define ES8388_ADCCONTROL9      0x11
#define ES8388_ADCCONTROL10     0x12
#define ES8388_ADCCONTROL11     0x13
#define ES8388_ADCCONTROL12     0x14
#define ES8388_ADCCONTROL13     0x15
#define ES8388_ADCCONTROL14     0x16

#define ES8388_DACCONTROL1      0x17
#define ES8388_DACCONTROL2      0x18
#define ES8388_DACCONTROL3      0x19
#define ES8388_DACCONTROL4      0x1A
#define ES8388_DACCONTROL5      0x1B
#define ES8388_DACCONTROL6      0x1C
#define ES8388_DACCONTROL7      0x1D
#define ES8388_DACCONTROL8      0x1E
#define ES8388_DACCONTROL9      0x1F
#define ES8388_DACCONTROL10     0x20
#define ES8388_DACCONTROL11     0x21
#define ES8388_DACCONTROL12     0x22
#define ES8388_DACCONTROL13     0x23
#define ES8388_DACCONTROL14     0x24
#define ES8388_DACCONTROL15     0x25
#define ES8388_DACCONTROL16     0x26
#define ES8388_DACCONTROL17     0x27
#define ES8388_DACCONTROL18     0x28
#define ES8388_DACCONTROL19     0x29
#define ES8388_DACCONTROL20     0x2A
#define ES8388_DACCONTROL21     0x2B
#define ES8388_DACCONTROL22     0x2C
#define ES8388_DACCONTROL23     0x2D
#define ES8388_DACCONTROL24     0x2E
#define ES8388_DACCONTROL25     0x2F
#define ES8388_DACCONTROL26     0x30
#define ES8388_DACCONTROL27     0x31
#define ES8388_DACCONTROL28     0x32
#define ES8388_DACCONTROL29     0x33
#define ES8388_DACCONTROL30     0x34

// ADC input selection for ADCCONTROL2 register
#define ADC_INPUT_LINPUT1_RINPUT1   0x00
#define ADC_INPUT_MIC1              0x05
#define ADC_INPUT_MIC2              0x06
#define ADC_INPUT_LINPUT2_RINPUT2   0x50  // LIN2/RIN2 for line input
#define ADC_INPUT_DIFFERENCE        0xF0

// DAC output selection for DACPOWER register (0x04)
// Bits 7-6: PdnDACL/R (0=power up, 1=power down)
// Bits 5-2: Output enables (1=enabled)
#define DAC_OUTPUT_ROUT2    0x04  // bit 2
#define DAC_OUTPUT_LOUT2    0x08  // bit 3
#define DAC_OUTPUT_ROUT1    0x10  // bit 4
#define DAC_OUTPUT_LOUT1    0x20  // bit 5
#define DAC_OUTPUT_ALL      0x3C  // All outputs enabled, DACs powered up

// I2S handles
static i2s_chan_handle_t i2s_tx_handle = NULL;
static i2s_chan_handle_t i2s_rx_handle = NULL;
static bool es8388_initialized = false;
static bool dac_enabled = false;

static esp_err_t es8388_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t data[2] = {reg, val};
    esp_err_t ret = ESP_FAIL;
    
    // Retry I2C write up to 3 times
    for (int i = 0; i < 3; i++) {
        ret = i2c_master_write_to_device(
            I2C_MASTER_NUM, ES8388_ADDR, data, 2, pdMS_TO_TICKS(100));
        if (ret == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C write failed: reg=0x%02x, val=0x%02x, err=%s", 
                 reg, val, esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t es8388_read_reg(uint8_t reg, uint8_t *val)
{
    esp_err_t ret = ESP_FAIL;
    
    // Retry I2C read up to 3 times
    for (int i = 0; i < 3; i++) {
        ret = i2c_master_write_read_device(
            I2C_MASTER_NUM, ES8388_ADDR, &reg, 1, val, 1, pdMS_TO_TICKS(100));
        if (ret == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C read failed: reg=0x%02x, err=%s", reg, esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t es8388_codec_init(bool enable_dac)
{
    esp_err_t res = ESP_OK;
    
    ESP_LOGI(TAG, "Initializing ES8388 codec (DAC=%s)", enable_dac ? "enabled" : "disabled");
    
    // Add larger delay to let MCLK stabilize before sending I2C commands
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // Reset codec to default state
    res |= es8388_write_reg(ES8388_DACCONTROL3, 0x04);  // Mute DAC during config
    res |= es8388_write_reg(ES8388_CONTROL2, 0x50);     // Low power references
    res |= es8388_write_reg(ES8388_CHIPPOWER, 0x00);    // Power up all blocks
    res |= es8388_write_reg(ES8388_MASTERMODE, 0x00);   // Slave mode (ESP32 is I2S master)
    
    // Configure DAC
    res |= es8388_write_reg(ES8388_DACPOWER, 0xC0);     // Disable DAC outputs initially
    res |= es8388_write_reg(ES8388_CONTROL1, 0x12);     // ADC+DAC mode
    res |= es8388_write_reg(ES8388_DACCONTROL1, 0x18);  // 16-bit I2S
    res |= es8388_write_reg(ES8388_DACCONTROL2, 0x02);  // DACFsMode=single speed, ratio=256
    res |= es8388_write_reg(ES8388_DACCONTROL16, 0x00); // Audio from I2S (not analog bypass)
    res |= es8388_write_reg(ES8388_DACCONTROL17, 0x90); // Left DAC to left mixer (LD2LO=1)
    res |= es8388_write_reg(ES8388_DACCONTROL18, 0x00); // Reserved/unused
    res |= es8388_write_reg(ES8388_DACCONTROL19, 0x00); // Reserved/unused  
    res |= es8388_write_reg(ES8388_DACCONTROL20, 0x90); // Right DAC to right mixer (RD2RO=1)
    res |= es8388_write_reg(ES8388_DACCONTROL21, 0x80); // ADC and DAC share LRCK
    res |= es8388_write_reg(ES8388_DACCONTROL23, 0x00); // VROI = 0
    
    // Set DAC volume to reasonable level
    res |= es8388_write_reg(ES8388_DACCONTROL4, 0x00);  // Left DAC volume 0dB
    res |= es8388_write_reg(ES8388_DACCONTROL5, 0x00);  // Right DAC volume 0dB
    
    // Configure ADC
    res |= es8388_write_reg(ES8388_ADCPOWER, 0xFF);     // Power off ADC first
    res |= es8388_write_reg(ES8388_ADCCONTROL1, 0x00);  // PGA gain = 0dB (no clipping at max laptop volume)
    
    // Select LIN2/RIN2 as input source (for line input from aux cable)
    res |= es8388_write_reg(ES8388_ADCCONTROL2, ADC_INPUT_LINPUT2_RINPUT2);
    
    res |= es8388_write_reg(ES8388_ADCCONTROL3, 0x02);  // DS: stereo
    res |= es8388_write_reg(ES8388_ADCCONTROL4, 0x0C);  // 16-bit I2S, left/right normal
    res |= es8388_write_reg(ES8388_ADCCONTROL5, 0x02);  // ADCFsMode=single speed, ratio=256
    
    // Set ADC volume
    res |= es8388_write_reg(ES8388_ADCCONTROL8, 0x00);  // Left ADC volume 0dB
    res |= es8388_write_reg(ES8388_ADCCONTROL9, 0x00);  // Right ADC volume 0dB
    
    // Power on ADC
    res |= es8388_write_reg(ES8388_ADCPOWER, 0x00);     // Power on ADC, enable L/R input
    
    // Enable DAC if requested (for COMBO mode headphone output)
    if (enable_dac) {
        // Set output volume BEFORE I2S starts (I2C fails after due to MCLK EMI)
        // Output volume scale: 0x00=-45dB, 0x1E=0dB, 0x21=+4.5dB (max)
        // Using max output gain (+4.5dB) for unity passthrough
        res |= es8388_write_reg(ES8388_DACCONTROL24, 0x21);  // LOUT1 volume +4.5dB
        res |= es8388_write_reg(ES8388_DACCONTROL25, 0x21);  // ROUT1 volume +4.5dB
        res |= es8388_write_reg(ES8388_DACCONTROL26, 0x21);  // LOUT2 volume +4.5dB
        res |= es8388_write_reg(ES8388_DACCONTROL27, 0x21);  // ROUT2 volume +4.5dB
        
        // Enable ALL outputs + power up DACs (bits 7-6 = 0 for DAC power on)
        res |= es8388_write_reg(ES8388_DACPOWER, DAC_OUTPUT_ALL);  // 0x3C
        // Unmute DAC
        res |= es8388_write_reg(ES8388_DACCONTROL3, 0x00);
        dac_enabled = true;
        ESP_LOGI(TAG, "DAC enabled for headphone output (+4.5dB)");
    } else {
        dac_enabled = false;
    }
    
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "ES8388 codec initialization failed");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "ES8388 codec initialized successfully");
    return ESP_OK;
}

static esp_err_t i2s_init(bool enable_dac)
{
    ESP_LOGI(TAG, "Initializing I2S for ES8388");
    
    // Channel configuration - create both TX and RX on same port
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    
    esp_err_t ret;
    
    if (enable_dac) {
        // Full duplex: both TX and RX
        ret = i2s_new_channel(&chan_cfg, &i2s_tx_handle, &i2s_rx_handle);
    } else {
        // RX only for TX mode (no local output needed)
        ret = i2s_new_channel(&chan_cfg, NULL, &i2s_rx_handle);
    }
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channel: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Standard mode configuration for ES8388
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = AUDIO_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,  // MCLK = 256 * Fs = 12.288 MHz
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = ES8388_MCLK_IO,
            .bclk = ES8388_BCLK_IO,
            .ws = ES8388_WS_IO,
            .dout = ES8388_DOUT_IO,
            .din = ES8388_DIN_IO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    
    // Initialize RX channel (always needed for audio input)
    ret = i2s_channel_init_std_mode(i2s_rx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S RX: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Initialize TX channel if DAC is enabled
    if (enable_dac && i2s_tx_handle) {
        ret = i2s_channel_init_std_mode(i2s_tx_handle, &std_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to init I2S TX: %s", esp_err_to_name(ret));
            return ret;
        }
    }
    
    // Enable channels
    ret = i2s_channel_enable(i2s_rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S RX: %s", esp_err_to_name(ret));
        return ret;
    }
    
    if (enable_dac && i2s_tx_handle) {
        ret = i2s_channel_enable(i2s_tx_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to enable I2S TX: %s", esp_err_to_name(ret));
            return ret;
        }
    }
    
    ESP_LOGI(TAG, "I2S initialized: %dHz, 16-bit stereo, MCLK=GPIO%d", 
             AUDIO_SAMPLE_RATE, ES8388_MCLK_IO);
    
    return ESP_OK;
}

esp_err_t es8388_audio_init(bool enable_dac)
{
    if (es8388_initialized) {
        ESP_LOGW(TAG, "ES8388 already initialized");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing ES8388 audio driver");
    
    // Note: I2C should already be initialized by display driver (shared bus)
    // If not, we would need to init it here
    
    // Small delay to ensure I2C bus is ready
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // Verify ES8388 is present on I2C bus
    uint8_t test_val;
    esp_err_t ret = es8388_read_reg(ES8388_CONTROL1, &test_val);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ES8388 not found on I2C bus at address 0x%02x", ES8388_ADDR);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "ES8388 detected, CONTROL1=0x%02x", test_val);
    
    // CRITICAL: Configure ES8388 codec registers BEFORE starting I2S
    // I2S drives MCLK on GPIO1 which creates EMI that disrupts I2C on GPIO5/6
    ret = es8388_codec_init(enable_dac);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ES8388 codec initialization failed");
        return ret;
    }
    
    // Now initialize I2S interface (starts driving MCLK/BCLK/WS to codec)
    ret = i2s_init(enable_dac);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S initialization failed");
        return ret;
    }
    
    es8388_initialized = true;
    ESP_LOGI(TAG, "ES8388 audio driver initialized successfully");
    
    return ESP_OK;
}

esp_err_t es8388_audio_deinit(void)
{
    if (!es8388_initialized) {
        return ESP_OK;
    }
    
    // Mute and power down codec
    es8388_write_reg(ES8388_DACCONTROL3, 0x04);  // Mute DAC
    es8388_write_reg(ES8388_DACPOWER, 0xC0);     // Disable outputs
    es8388_write_reg(ES8388_ADCPOWER, 0xFF);     // Power off ADC
    es8388_write_reg(ES8388_CHIPPOWER, 0xFF);    // Power off all
    
    // Disable and delete I2S channels
    if (i2s_rx_handle) {
        i2s_channel_disable(i2s_rx_handle);
        i2s_del_channel(i2s_rx_handle);
        i2s_rx_handle = NULL;
    }
    
    if (i2s_tx_handle) {
        i2s_channel_disable(i2s_tx_handle);
        i2s_del_channel(i2s_tx_handle);
        i2s_tx_handle = NULL;
    }
    
    es8388_initialized = false;
    dac_enabled = false;
    
    ESP_LOGI(TAG, "ES8388 audio driver deinitialized");
    return ESP_OK;
}

esp_err_t es8388_audio_read_stereo(int16_t *stereo_buffer, size_t max_frames, size_t *frames_read)
{
    if (!es8388_initialized || !i2s_rx_handle) {
        ESP_LOGE(TAG, "ES8388 not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!stereo_buffer || !frames_read) {
        return ESP_ERR_INVALID_ARG;
    }
    
    size_t bytes_read = 0;
    size_t bytes_to_read = max_frames * 2 * sizeof(int16_t);  // stereo = 2 channels
    
    esp_err_t ret = i2s_channel_read(i2s_rx_handle, stereo_buffer, bytes_to_read, 
                                      &bytes_read, pdMS_TO_TICKS(10));
    
    if (ret == ESP_ERR_TIMEOUT) {
        *frames_read = 0;
        return ESP_OK;  // No data yet, not an error
    }
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S read failed: %s", esp_err_to_name(ret));
        *frames_read = 0;
        return ret;
    }
    
    *frames_read = bytes_read / (2 * sizeof(int16_t));
    return ESP_OK;
}

// Track consecutive I2S TX errors for recovery
static int i2s_tx_error_count = 0;
#define I2S_TX_ERROR_THRESHOLD 3

esp_err_t es8388_audio_write_stereo(const int16_t *stereo_buffer, size_t frames)
{
    if (!es8388_initialized || !dac_enabled || !i2s_tx_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!stereo_buffer || frames == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    size_t bytes_written = 0;
    size_t bytes_to_write = frames * 2 * sizeof(int16_t);  // stereo = 2 channels
    
    // Use 20ms timeout to prevent blocking indefinitely
    esp_err_t ret = i2s_channel_write(i2s_tx_handle, stereo_buffer, bytes_to_write,
                                       &bytes_written, pdMS_TO_TICKS(20));
    
    if (ret == ESP_ERR_TIMEOUT) {
        i2s_tx_error_count++;
        
        // After several consecutive timeouts, try to recover I2S channel
        if (i2s_tx_error_count >= I2S_TX_ERROR_THRESHOLD) {
            ESP_LOGW(TAG, "I2S TX timeout x%d, recovering channel...", i2s_tx_error_count);
            
            // Disable and re-enable the I2S TX channel to clear stuck state
            i2s_channel_disable(i2s_tx_handle);
            vTaskDelay(pdMS_TO_TICKS(5));
            i2s_channel_enable(i2s_tx_handle);
            
            i2s_tx_error_count = 0;
            ESP_LOGI(TAG, "I2S TX channel recovered");
        }
        return ESP_ERR_TIMEOUT;
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Success - reset error counter
    i2s_tx_error_count = 0;
    return ESP_OK;
}

esp_err_t es8388_audio_set_volume(uint8_t volume)
{
    if (!es8388_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Volume is 0-100, map to 0-33 register range (0dB to -33dB)
    // Lower register value = higher volume
    uint8_t reg_val = (100 - volume) * 33 / 100;
    
    // Note: I2C may fail after I2S starts due to MCLK EMI on GPIO1
    // Volume control is non-critical - audio works at default volume if this fails
    esp_err_t ret1 = es8388_write_reg(ES8388_DACCONTROL24, reg_val);  // LOUT1 volume
    esp_err_t ret2 = es8388_write_reg(ES8388_DACCONTROL25, reg_val);  // ROUT1 volume
    
    if (ret1 == ESP_OK && ret2 == ESP_OK) {
        ESP_LOGI(TAG, "Volume set to %d%% (reg=0x%02x)", volume, reg_val);
    } else {
        ESP_LOGW(TAG, "Volume set failed (I2C error after I2S start), using default");
    }
    
    return ESP_OK;  // Non-fatal - audio still works
}

esp_err_t es8388_audio_set_input_gain(uint8_t gain_db)
{
    if (!es8388_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Gain is 0-24dB, map to register value
    // PGA gain: 0x00=0dB, 0x88=24dB (in 3dB steps per nibble)
    uint8_t gain_nibble = (gain_db / 3) & 0x0F;
    uint8_t reg_val = (gain_nibble << 4) | gain_nibble;  // Same for L and R
    
    esp_err_t ret = es8388_write_reg(ES8388_ADCCONTROL1, reg_val);
    
    ESP_LOGI(TAG, "Input gain set to %ddB (reg=0x%02x)", gain_db, reg_val);
    return ret;
}

bool es8388_audio_is_ready(void)
{
    return es8388_initialized;
}
