/**
 * MeshNet Audio RX Node
 * 
 * Receives Opus-compressed audio from mesh network and plays via I2S DAC
 * Uses ESP-ADF pipeline with Opus decoding
 */

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include "config/build.h"
#include "config/pins.h"
#include "control/display.h"
#include "control/buttons.h"
#include "control/status.h"
#include "network/mesh_net.h"
#include "audio/i2s_audio.h"
#include "audio/adf_pipeline.h"
#include <string.h>
#include <netinet/in.h>

static const char *TAG = "rx_main";

static rx_status_t status = {
    .rssi = -100,
    .latency_ms = 0,
    .hops = 1,
    .receiving_audio = false,
    .bandwidth_kbps = 0
};

static display_view_t current_view = DISPLAY_VIEW_NETWORK;
static adf_pipeline_handle_t rx_pipeline = NULL;

// Packet tracking for statistics
static uint32_t packets_received = 0;
static uint32_t dropped_packets = 0;
static uint16_t last_seq = 0;
static bool first_packet = true;
static uint32_t last_packet_time = 0;

// Audio callback for mesh network - called when audio frames are received
static void audio_rx_callback(const uint8_t *payload, size_t len, uint16_t seq, uint32_t timestamp) {
    // Track sequence gaps for packet loss measurement
    if (!first_packet) {
        uint16_t expected_seq = (last_seq + 1) & 0xFFFF;
        if (seq != expected_seq) {
            int16_t gap = (int16_t)(seq - expected_seq);
            if (gap > 0 && gap < 100) {
                dropped_packets += gap;
            }
        }
    }
    first_packet = false;
    last_seq = seq;
    
    // Feed Opus data to the pipeline
    if (rx_pipeline) {
        esp_err_t ret = adf_pipeline_feed_opus(rx_pipeline, payload, len, seq, timestamp);
        if (ret == ESP_OK) {
            status.receiving_audio = true;
            packets_received++;
            last_packet_time = xTaskGetTickCount();
            
            if ((packets_received & 0x7F) == 0) {
                ESP_LOGI(TAG, "RX packet %lu (seq=%u, len=%zu)", packets_received, seq, len);
            }
        } else {
            ESP_LOGW(TAG, "Pipeline buffer full, dropping packet seq=%u", seq);
        }
    }
}

void app_main(void) {
    esp_log_level_set("*", ESP_LOG_INFO);
    ESP_LOGI(TAG, "======================================");
    ESP_LOGI(TAG, "MeshNet Audio RX starting (Opus)...");
    ESP_LOGI(TAG, "Build: " __DATE__ " " __TIME__);
    ESP_LOGI(TAG, "Audio: %dHz, %d-bit, %dms frames",
             AUDIO_SAMPLE_RATE, AUDIO_BITS_PER_SAMPLE, AUDIO_FRAME_MS);
    ESP_LOGI(TAG, "======================================");

    // Initialize control layer
    if (display_init() != ESP_OK) {
        ESP_LOGW(TAG, "Display init failed, continuing without display");
    }
    ESP_ERROR_CHECK(buttons_init());

    // Initialize audio output (UDA1334 or similar I2S DAC)
    ESP_ERROR_CHECK(i2s_audio_init());

    // Create RX pipeline with Opus decoding BEFORE network init
    // Opus decoder needs ~12KB heap - allocate before WiFi/mesh consumes heap
    ESP_LOGI(TAG, "Creating audio pipeline (heap: %lu bytes)...",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    
    adf_pipeline_config_t pipeline_cfg = ADF_PIPELINE_CONFIG_DEFAULT();
    pipeline_cfg.type = ADF_PIPELINE_RX;
    pipeline_cfg.enable_local_output = false;
    
    rx_pipeline = adf_pipeline_create(&pipeline_cfg);
    if (!rx_pipeline) {
        ESP_LOGE(TAG, "Failed to create RX pipeline");
        return;
    }
    
    ESP_LOGI(TAG, "Audio pipeline created (heap: %lu bytes remaining)",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));

    // Initialize network layer (ESP-WIFI-MESH mode) - after audio pipeline
    // WiFi/mesh stack consumes significant heap (~40KB)
    ESP_LOGI(TAG, "Starting mesh network...");
    ESP_ERROR_CHECK(network_init_mesh());
    ESP_ERROR_CHECK(network_start_latency_measurement());

    // Register audio callback for mesh audio reception
    ESP_ERROR_CHECK(network_register_audio_callback(audio_rx_callback));

    ESP_LOGI(TAG, "RX initialized, waiting for network...");

    // Wait for network to be stream-ready via event notification
    ESP_ERROR_CHECK(network_register_startup_notification(xTaskGetCurrentTaskHandle()));
    uint32_t notify_value = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (notify_value > 0) {
        ESP_LOGI(TAG, "Network ready - starting audio pipeline");
    }

    // Start the RX pipeline
    ESP_ERROR_CHECK(adf_pipeline_start(rx_pipeline));

    ESP_LOGI(TAG, "Main task stack high water mark: %u bytes", uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGI(TAG, "Free heap: %u bytes", heap_caps_get_free_size(MALLOC_CAP_8BIT));

    uint32_t last_stats_update = xTaskGetTickCount();
    
    while (1) {
        // Handle button events
        button_event_t btn_event = buttons_poll();
        if (btn_event == BUTTON_EVENT_SHORT_PRESS) {
            current_view = (current_view == DISPLAY_VIEW_NETWORK) ? 
                          DISPLAY_VIEW_AUDIO : DISPLAY_VIEW_NETWORK;
            ESP_LOGI(TAG, "View changed to %s", 
                    current_view == DISPLAY_VIEW_NETWORK ? "Network" : "Audio");
        }
        
        // Check for audio stream timeout
        if ((xTaskGetTickCount() - last_packet_time) > pdMS_TO_TICKS(100)) {
            status.receiving_audio = false;
        }
        
        // Update network stats every second
        uint32_t now = xTaskGetTickCount();
        if ((now - last_stats_update) >= pdMS_TO_TICKS(1000)) {
            status.rssi = network_get_rssi();
            status.latency_ms = network_get_latency_ms();
            
            // Get pipeline stats
            adf_pipeline_stats_t stats;
            if (adf_pipeline_get_stats(rx_pipeline, &stats) == ESP_OK) {
                // Estimate bandwidth: packets × ~100 bytes × 8 bits / 1000 = kbps
                status.bandwidth_kbps = (packets_received * 100 * 8) / 1000;
                
                // Log stats
                float loss_pct = 0.0f;
                if (packets_received + dropped_packets > 0) {
                    loss_pct = (100.0f * dropped_packets) / (packets_received + dropped_packets);
                }
                ESP_LOGI(TAG, "Stats: RX=%lu, DROP=%lu (%.1f%%), underrun=%lu, dec=%luus, buf=%u%%", 
                         packets_received, dropped_packets, loss_pct,
                         stats.buffer_underruns, stats.avg_decode_time_us,
                         stats.buffer_fill_percent);
            }
            
            last_stats_update = now;
        }
        
        // Update display at 10 Hz
        static uint32_t last_display_update = 0;
        if ((now - last_display_update) >= pdMS_TO_TICKS(100)) {
            display_render_rx(current_view, &status);
            last_display_update = now;
        }
        
        // Small delay to prevent tight loop
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
