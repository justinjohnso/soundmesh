/**
 * MeshNet Audio SRC Node (Zero Portal)
 * 
 * Source capture + local headphone monitor via ES8388.
 * Uses ESP-ADF pipeline with Opus compression for mesh transmission.
 */

#if defined(CONFIG_SRC_BUILD)

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_log.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <esp_mesh.h>
#include <string.h>
#include <math.h>
#include "config/build.h"
#include "config/pins.h"
#include "control/display.h"
#include "control/buttons.h"
#include "control/status.h"
#include "audio/tone_gen.h"
#include "audio/adf_pipeline.h"
#include "network/mesh_net.h"
#include "control/serial_dashboard.h"
#include "control/memory_monitor.h"

#ifdef CONFIG_USE_ES8388
#include "audio/es8388_audio.h"
#else
#include "audio/adc_audio.h"
#include "audio/i2s_audio.h"
#endif

static const char *TAG = "src_main";

static src_status_t status = {
    .input_mode = INPUT_MODE_AUX,
    .audio_active = false,
    .input_peak = 0,
    .usb_ready = false,
    .usb_active = false,
    .usb_fallback_to_aux = false,
    .connected_nodes = 0,
    .bandwidth_kbps = 0,
    .tone_freq_hz = 440,
    .output_volume = 1.0f,
    .nearest_rssi = -100
};

static display_view_t current_view = DISPLAY_VIEW_AUDIO;
static adf_pipeline_handle_t tx_pipeline = NULL;

static input_mode_t status_input_mode_from_adf(adf_input_mode_t mode) {
    if (mode == ADF_INPUT_MODE_USB) return INPUT_MODE_USB;
    if (mode == ADF_INPUT_MODE_TONE) return INPUT_MODE_TONE;
    return INPUT_MODE_AUX;
}

static void update_status_from_pipeline_stats(const adf_pipeline_stats_t *stats) {
    if (!stats) return;
    status.input_mode = status_input_mode_from_adf(adf_pipeline_get_input_mode(tx_pipeline));
    status.audio_active = stats->input_signal_present;
    status.input_peak = stats->input_peak;
}

void app_main(void) {
    ESP_LOGI(TAG, "======================================");
    ESP_LOGI(TAG, "MeshNet Audio SRC starting (Zero Portal)...");
    ESP_LOGI(TAG, "======================================");

    ESP_ERROR_CHECK(memory_monitor_init());

    // CRITICAL: Display init installs the shared I2C driver on GPIO 5/6
    if (display_init() != ESP_OK) {
        ESP_LOGW(TAG, "Display init failed");
    }
    // Small delay to let I2C bus settle
    vTaskDelay(pdMS_TO_TICKS(100));

#ifdef CONFIG_USE_ES8388
    if (es8388_audio_init(true) != ESP_OK) {
        ESP_LOGW(TAG, "ES8388 init failed");
    }
#else
    ESP_ERROR_CHECK(adc_audio_init());
    ESP_ERROR_CHECK(i2s_audio_init());
#endif

    ESP_ERROR_CHECK(buttons_init());
    ESP_ERROR_CHECK(tone_gen_init(status.tone_freq_hz));

    adf_pipeline_config_t pipeline_cfg = ADF_PIPELINE_CONFIG_DEFAULT();
    pipeline_cfg.type = ADF_PIPELINE_TX;
    pipeline_cfg.enable_local_output = true;
    
    tx_pipeline = adf_pipeline_create(&pipeline_cfg);
    if (!tx_pipeline) return;
    
    ESP_ERROR_CHECK(adf_pipeline_start(tx_pipeline));

    ESP_LOGI(TAG, "Starting mesh network...");
    ESP_ERROR_CHECK(network_init_mesh());

    esp_task_wdt_init(&(esp_task_wdt_config_t){
        .timeout_ms = 5000,
        .idle_core_mask = 0,
        .trigger_panic = true,
    });
    esp_task_wdt_add(NULL);

    dashboard_init();
    ESP_ERROR_CHECK(network_register_startup_notification(xTaskGetCurrentTaskHandle()));
    uint32_t notify_value = 0;
    while (notify_value == 0) {
        notify_value = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        esp_task_wdt_reset();
    }

    memory_monitor_start_periodic(10000);
    int64_t last_status_ms = 0;
    int64_t last_display_ms = 0;

    while (1) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(10));
        
        int64_t now_ms = esp_timer_get_time() / 1000;
        
        if (last_status_ms == 0 || (now_ms - last_status_ms) >= 1000) {
            status.connected_nodes = network_get_connected_nodes();
            adf_pipeline_stats_t stats;
            if (adf_pipeline_get_stats(tx_pipeline, &stats) == ESP_OK) {
                update_status_from_pipeline_stats(&stats);
                uint32_t tx_bytes = network_get_tx_bytes_and_reset();
                status.bandwidth_kbps = (tx_bytes * 8) / 1000;
            }
            dashboard_render_src(&status);
            last_status_ms = now_ms;
        }

        if (last_display_ms == 0 || (now_ms - last_display_ms) >= 100) {
            display_render_src(current_view, &status);
            last_display_ms = now_ms;
        }
    }
}

#endif
