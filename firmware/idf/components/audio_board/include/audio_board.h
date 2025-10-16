/**
 * @brief Minimal audio_board stub header for XIAO-ESP32-S3 MVP
 * This is a minimal stub that provides the audio_board API without ESP-ADF dependencies
 */

#pragma once

#include "esp_err.h"

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

#ifdef __cplusplus
}
#endif