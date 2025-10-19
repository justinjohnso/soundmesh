#include "control/display.h"
#include "config/pins.h"
#include <esp_log.h>
#include <string.h>

static const char *TAG = "display";

esp_err_t display_init(void) {
    ESP_LOGI(TAG, "Display init (stub)");
    // TODO: Initialize SSD1306 via I2C
    return ESP_OK;
}

void display_clear(void) {
    // TODO: Clear display
}

void display_render_tx(display_view_t view, const tx_status_t *status) {
    if (view == DISPLAY_VIEW_NETWORK) {
        ESP_LOGI(TAG, "TX Network View: %lu nodes", status->connected_nodes);
        // TODO: Render network view showing connected nodes
    } else {
        ESP_LOGI(TAG, "TX Audio View: mode=%d, active=%d", status->input_mode, status->audio_active);
        // TODO: Render audio view showing input mode and status
    }
}

void display_render_rx(display_view_t view, const rx_status_t *status) {
    if (view == DISPLAY_VIEW_NETWORK) {
        ESP_LOGI(TAG, "RX Network View: RSSI=%d, latency=%lums", status->rssi, status->latency_ms);
        // TODO: Render network stats
    } else {
        ESP_LOGI(TAG, "RX Audio View: receiving=%d, bandwidth=%lukbps", status->receiving_audio, status->bandwidth_kbps);
        // TODO: Render audio stats
    }
}
