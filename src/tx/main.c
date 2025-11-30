/**
 * MeshNet Audio TX Node
 * 
 * Pure TX mode: ES8388 input → Opus encode → mesh broadcast
 * No local audio output (use COMBO for headphone monitoring)
 */

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_log.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <string.h>
#include <math.h>
#include "config/build.h"
#include "config/pins.h"
#include "control/display.h"
#include "control/buttons.h"
#include "control/status.h"
#include "network/mesh_net.h"
#include "audio/tone_gen.h"
#include "audio/usb_audio.h"
#include "audio/adf_pipeline.h"

#ifdef CONFIG_USE_ES8388
#include "audio/es8388_audio.h"
#else
#include "audio/adc_audio.h"
#endif

static const char *TAG = "tx_main";

static tx_status_t status = {
    .input_mode = INPUT_MODE_AUX,
    .audio_active = false,
    .connected_nodes = 0,
    .latency_ms = 10,
    .bandwidth_kbps = 0,
    .rssi = 0,
    .tone_freq_hz = 440
};

static display_view_t current_view = DISPLAY_VIEW_NETWORK;
static adf_pipeline_handle_t tx_pipeline = NULL;

void update_tone_oscillate(int64_t now_ms) {
    static int64_t last_log_ms = 0;
    
    uint32_t phase = ((uint32_t)(now_ms / 20)) % 200;
    float ratio = (float)phase / 200.0f;
    float sine_val = sinf(ratio * 2.0f * M_PI);
    uint32_t center = 500;
    uint32_t range = 200;
    uint32_t new_freq = center + (uint32_t)(sine_val * range);
    
    if (abs((int)new_freq - (int)status.tone_freq_hz) > 5) {
        status.tone_freq_hz = new_freq;
        tone_gen_set_frequency(status.tone_freq_hz);
    }
    
    if ((now_ms - last_log_ms) > 2000) {
        ESP_LOGI(TAG, "Tone oscillating: freq=%lu Hz", status.tone_freq_hz);
        last_log_ms = now_ms;
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "======================================");
    ESP_LOGI(TAG, "MeshNet Audio TX starting (Opus)...");
    ESP_LOGI(TAG, "Build: " __DATE__ " " __TIME__);
    ESP_LOGI(TAG, "Audio: %dHz, %d-bit, %dms frames, Opus %d kbps",
             AUDIO_SAMPLE_RATE, AUDIO_BITS_PER_SAMPLE, AUDIO_FRAME_MS, OPUS_BITRATE / 1000);
    ESP_LOGI(TAG, "======================================");

#ifdef CONFIG_USE_ES8388
    ESP_LOGI(TAG, "Audio input: ES8388 codec (LIN2/RIN2)");
#else
    ESP_LOGI(TAG, "Audio input: ADC (legacy)");
#endif

    // Initialize control layer
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(buttons_init());

    // Initialize network layer (ESP-WIFI-MESH)
    ESP_LOGI(TAG, "Starting mesh network...");
    ESP_ERROR_CHECK(network_init_mesh());

    // Initialize audio sources
    ESP_ERROR_CHECK(tone_gen_init(status.tone_freq_hz));
    ESP_ERROR_CHECK(usb_audio_init());

#ifdef CONFIG_USE_ES8388
    // TX mode: ES8388 ADC only (no DAC output)
    ESP_ERROR_CHECK(es8388_audio_init(false));
#else
    ESP_ERROR_CHECK(adc_audio_init());
#endif

    // Create TX pipeline with Opus encoding (no local output)
    adf_pipeline_config_t pipeline_cfg = ADF_PIPELINE_CONFIG_DEFAULT();
    pipeline_cfg.type = ADF_PIPELINE_TX;
    pipeline_cfg.enable_local_output = false;  // TX only - no headphones
    pipeline_cfg.opus_bitrate = OPUS_BITRATE;
    pipeline_cfg.opus_complexity = OPUS_COMPLEXITY;
    
    tx_pipeline = adf_pipeline_create(&pipeline_cfg);
    if (!tx_pipeline) {
        ESP_LOGE(TAG, "Failed to create TX pipeline");
        return;
    }

    // Initialize watchdog
    esp_err_t wdt_err = esp_task_wdt_init(&(esp_task_wdt_config_t){
        .timeout_ms = 5000,
        .idle_core_mask = 0,
        .trigger_panic = true,
    });
    if (wdt_err == ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    } else {
        ESP_ERROR_CHECK(wdt_err);
        ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    }

    ESP_LOGI(TAG, "TX initialized, waiting for network...");

    // Wait for network to be ready
    ESP_ERROR_CHECK(network_register_startup_notification(xTaskGetCurrentTaskHandle()));
    uint32_t notify_value = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (notify_value > 0) {
        ESP_LOGI(TAG, "Network ready - starting audio pipeline");
    }

    // Start the TX pipeline
    ESP_ERROR_CHECK(adf_pipeline_start(tx_pipeline));
    status.audio_active = true;

    ESP_LOGI(TAG, "Main task stack high water mark: %u bytes", uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGI(TAG, "Free heap: %u bytes", heap_caps_get_free_size(MALLOC_CAP_8BIT));

    // Main control loop
    int64_t last_button_ms = esp_timer_get_time() / 1000;
    int64_t last_display_ms = last_button_ms;
    int64_t last_stats_ms = last_button_ms;

    while (1) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(10));
        
        int64_t now_ms = esp_timer_get_time() / 1000;
        
        // Button polling every 5ms
        if (now_ms - last_button_ms >= 5) {
            last_button_ms = now_ms;
            button_event_t btn_event = buttons_poll();
            if (btn_event == BUTTON_EVENT_SHORT_PRESS) {
                current_view = (current_view == DISPLAY_VIEW_NETWORK) ?
                              DISPLAY_VIEW_AUDIO : DISPLAY_VIEW_NETWORK;
                ESP_LOGI(TAG, "View changed to %s",
                        current_view == DISPLAY_VIEW_NETWORK ? "Network" : "Audio");
            } else if (btn_event == BUTTON_EVENT_LONG_PRESS) {
                status.input_mode = (status.input_mode + 1) % 3;
                ESP_LOGI(TAG, "Input mode changed to %d", status.input_mode);
            }
        }
        
        // Tone oscillation for test mode
        if (status.input_mode == INPUT_MODE_TONE) {
            update_tone_oscillate(now_ms);
        }

        // Stats update every 1000ms
        if (now_ms - last_stats_ms >= 1000) {
            status.connected_nodes = network_get_connected_nodes();
            status.rssi = network_get_rssi();
            status.latency_ms = network_get_latency_ms();
            
            // Get pipeline stats
            adf_pipeline_stats_t stats;
            if (adf_pipeline_get_stats(tx_pipeline, &stats) == ESP_OK) {
                status.bandwidth_kbps = (stats.frames_processed * 100 * 8) / 1000;
                
                ESP_LOGI(TAG, "Stats: nodes=%lu, rssi=%d, frames=%lu, drops=%lu, enc=%luus",
                         status.connected_nodes, status.rssi,
                         stats.frames_processed, stats.frames_dropped,
                         stats.avg_encode_time_us);
            }
            
            last_stats_ms = now_ms;
        }

        // Display update every 100ms
        if (now_ms - last_display_ms >= 100) {
            last_display_ms = now_ms;
            display_render_tx(current_view, &status);
        }
    }
}
