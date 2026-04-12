#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize USB audio runtime resources.
 */
esp_err_t usb_audio_init(void);

/**
 * Release USB audio runtime resources.
 */
esp_err_t usb_audio_deinit(void);

/**
 * Read interleaved stereo PCM frames from USB capture runtime.
 * Returns ESP_ERR_NOT_FOUND when insufficient buffered data is available.
 */
esp_err_t usb_audio_read_stereo(int16_t *stereo_buffer, size_t max_frames, size_t *frames_read);

/**
 * Runtime state helpers used by TX input mode policy.
 */
bool usb_audio_is_ready(void);
bool usb_audio_is_active(void);
uint32_t usb_audio_get_inactive_ms(void);

#ifdef __cplusplus
}
#endif
