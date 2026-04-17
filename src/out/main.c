/**
 * MeshNet Audio OUT Node (Zero Portal)
 * 
 * Receives Opus-compressed audio from mesh network and plays via I2S DAC.
 * Uses ESP-ADF pipeline with Opus decoding.
 */

#if defined(CONFIG_OUT_BUILD)

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <string.h>
#include "config/build.h"
#include "control/display.h"
#include "control/status.h"
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

static const char *TAG = "out_main";

static out_status_t status = {
    .receiving_audio = false,
    .rssi = -100,
    .loss_pct = 0.0f,
    .buffer_pct = 0,
    .bandwidth_kbps = 0,
    .battery_pct = 0
};

static display_view_t current_view = DISPLAY_VIEW_AUDIO;
static adf_pipeline_handle_t rx_pipeline = NULL;
static uint32_t last_rx_audio_forwarded = 0;

static void on_audio_rx(const uint8_t *payload, size_t len, uint16_t seq, uint32_t ts, const char *src_id) {
    if (rx_pipeline) {
        adf_pipeline_feed_opus(rx_pipeline, payload, len, seq, ts);
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "======================================");
    ESP_LOGI(TAG, "MeshNet Audio OUT starting (Zero Portal)...");
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

    adf_pipeline_config_t pipeline_cfg = ADF_PIPELINE_CONFIG_DEFAULT();
    pipeline_cfg.type = ADF_PIPELINE_RX;
    
    rx_pipeline = adf_pipeline_create(&pipeline_cfg);
    if (!rx_pipeline) return;
    
    ESP_ERROR_CHECK(adf_pipeline_start(rx_pipeline));

    ESP_LOGI(TAG, "Starting mesh network...");
    ESP_ERROR_CHECK(network_init_mesh());
    
    // Register callback so mesh packets reach the pipeline
    network_register_audio_callback(on_audio_rx);

    esp_task_wdt_init(&(esp_task_wdt_config_t){
        .timeout_ms = 5000,
        .idle_core_mask = 0,
        .trigger_panic = true,
    });
    esp_task_wdt_add(NULL);

    dashboard_init();
    memory_monitor_start_periodic(10000);
    int64_t last_status_ms = 0;
    int64_t last_display_ms = 0;

    while (1) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(10));
        
        int64_t now_ms = esp_timer_get_time() / 1000;
        
        if (last_status_ms == 0 || (now_ms - last_status_ms) >= 1000) {
            status.rssi = network_get_rssi();
            
            adf_pipeline_stats_t stats;
            if (adf_pipeline_get_stats(rx_pipeline, &stats) == ESP_OK) {
                status.buffer_pct = stats.buffer_fill_percent;
            }
            
            network_transport_stats_t tstats;
            if (network_get_transport_stats(&tstats) == ESP_OK) {
                status.receiving_audio = (tstats.rx_audio_forwarded > last_rx_audio_forwarded);
                last_rx_audio_forwarded = tstats.rx_audio_forwarded;
            }
            dashboard_render_out(&status);
            last_status_ms = now_ms;
        }

        if (last_display_ms == 0 || (now_ms - last_display_ms) >= 100) {
            display_render_out(current_view, &status);
            last_display_ms = now_ms;
        }
    }
}

#endif
