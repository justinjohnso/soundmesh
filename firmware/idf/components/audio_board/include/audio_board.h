/**
 * @brief Minimal audio_board stub header for XIAO-ESP32-S3 MVP
 */

#pragma once

#include "esp_err.h"
#include "esp_peripherals.h"
#include "audio_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void* audio_board_handle_t;

/**
 * @brief Initialize audio board stub
 * @return Handle to audio board
 */
audio_board_handle_t audio_board_init(void);

/**
 * @brief Deinitialize audio board stub
 * @param audio_board Handle to audio board
 * @return ESP_OK on success
 */
esp_err_t audio_board_deinit(audio_board_handle_t audio_board);

/**
 * @brief SD card initialization stub (not supported)
 * @param set Peripheral set handle
 * @param mode SD card mode
 * @return ESP_ERR_NOT_SUPPORTED
 */
esp_err_t audio_board_sdcard_init(esp_periph_set_handle_t set, periph_sdcard_mode_t mode);

/**
 * @brief Get audio HAL handle stub (not supported)
 * @return NULL
 */
audio_hal_handle_t audio_board_get_hal(void);

#ifdef __cplusplus
}
#endif