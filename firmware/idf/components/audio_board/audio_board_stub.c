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