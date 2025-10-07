/**
 * @brief Minimal audio_board stub for XIAO-ESP32-S3 MVP
 * 
 * This provides the minimal interface needed by ESP-ADF audio_stream components
 * without the complex board-specific GPIO dependencies of the full audio_board.
 */

#include "audio_board.h"
#include "esp_log.h"

static const char *TAG = "audio_board_stub";

typedef struct {
    int dummy;
} audio_board_t;

static audio_board_t s_board = {0};

audio_board_handle_t audio_board_init(void)
{
    ESP_LOGI(TAG, "Audio board stub initialized for XIAO-ESP32-S3");
    return &s_board;
}

esp_err_t audio_board_deinit(audio_board_handle_t audio_board)
{
    ESP_LOGI(TAG, "Audio board stub deinitialized");
    return ESP_OK;
}

esp_err_t audio_board_sdcard_init(esp_periph_set_handle_t set, periph_sdcard_mode_t mode)
{
    ESP_LOGW(TAG, "SD card not supported in stub implementation");
    return ESP_ERR_NOT_SUPPORTED;
}

audio_hal_handle_t audio_board_get_hal(void)
{
    ESP_LOGW(TAG, "Audio HAL not supported in stub implementation");
    return NULL;
}