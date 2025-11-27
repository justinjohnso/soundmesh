#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ES8388 Audio Codec Driver for PCBArtists ES8388 Module
 * 
 * This driver provides a simple interface for the ES8388 codec,
 * supporting simultaneous ADC (line input) and DAC (headphone output).
 * 
 * Hardware connections (XIAO ESP32-S3):
 *   I2C: SDA=GPIO5, SCL=GPIO6 (shared with OLED)
 *   I2S: MCLK=GPIO1, BCLK=GPIO7, WS=GPIO8, DOUT=GPIO9, DIN=GPIO2
 *   
 * Audio format: 48kHz, 16-bit stereo I2S
 */

/**
 * Initialize the ES8388 codec and I2S interface
 * 
 * @param enable_dac If true, enable DAC for headphone output (COMBO mode)
 *                   If false, only enable ADC for input capture (TX mode)
 * @return ESP_OK on success
 */
esp_err_t es8388_audio_init(bool enable_dac);

/**
 * Deinitialize the ES8388 codec and I2S interface
 * @return ESP_OK on success
 */
esp_err_t es8388_audio_deinit(void);

/**
 * Read stereo audio samples from ES8388 ADC (line input)
 * 
 * @param stereo_buffer Output buffer for interleaved L/R samples
 * @param max_frames Maximum number of stereo frames to read
 * @param frames_read Output: actual number of frames read
 * @return ESP_OK on success, ESP_ERR_TIMEOUT if no data available
 */
esp_err_t es8388_audio_read_stereo(int16_t *stereo_buffer, size_t max_frames, size_t *frames_read);

/**
 * Write stereo audio samples to ES8388 DAC (headphone output)
 * 
 * @param stereo_buffer Input buffer with interleaved L/R samples
 * @param frames Number of stereo frames to write
 * @return ESP_OK on success
 */
esp_err_t es8388_audio_write_stereo(const int16_t *stereo_buffer, size_t frames);

/**
 * Set the DAC output volume
 * 
 * @param volume Volume level 0-100 (0 = mute, 100 = max)
 * @return ESP_OK on success
 */
esp_err_t es8388_audio_set_volume(uint8_t volume);

/**
 * Set the ADC input gain
 * 
 * @param gain_db Gain in dB (0-24 dB typically)
 * @return ESP_OK on success
 */
esp_err_t es8388_audio_set_input_gain(uint8_t gain_db);

/**
 * Check if the ES8388 is initialized and ready
 * @return true if ready
 */
bool es8388_audio_is_ready(void);

#ifdef __cplusplus
}
#endif
