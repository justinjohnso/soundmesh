#include "ctrl_plane.h"
#include "esp_log.h"

static const char *TAG = "ctrl_plane";

esp_err_t ctrl_plane_init(void) {
    ESP_LOGI(TAG, "init (stub)");
    return ESP_OK;
}

esp_err_t ctrl_plane_announce(const node_announce_t *ann) {
    (void)ann;
    ESP_LOGI(TAG, "announce (stub)");
    return ESP_OK;
}

esp_err_t ctrl_plane_request_tx(void) {
    ESP_LOGI(TAG, "request_tx (stub)");
    return ESP_OK;
}
