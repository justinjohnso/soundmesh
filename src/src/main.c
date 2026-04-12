/**
 * MeshNet Audio SRC Node
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
#include "control/usb_portal.h"
#include "control/portal_ota.h"
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
    if (mode == ADF_INPUT_MODE_USB) {
        return INPUT_MODE_USB;
    }
    if (mode == ADF_INPUT_MODE_TONE) {
        return INPUT_MODE_TONE;
    }
    return INPUT_MODE_AUX;
}

static void update_status_from_pipeline_stats(const adf_pipeline_stats_t *stats) {
    if (!stats) {
        return;
    }
    status.input_mode = status_input_mode_from_adf(adf_pipeline_get_input_mode());
    status.audio_active = stats->input_signal_present;
    status.usb_ready = stats->usb_input_ready;
    status.usb_active = stats->usb_input_active;
    status.usb_fallback_to_aux = stats->usb_fallback_to_aux;
}

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
    ESP_LOGI(TAG, "MeshNet Audio SRC starting (Opus)...");
    ESP_LOGI(TAG, "Build: " __DATE__ " " __TIME__);
    ESP_LOGI(TAG,
             "Audio: %dHz, boundary=%d-bit (internal=%d-bit), frame=%dms (target=%dms fallback=%d), Opus %d kbps",
             AUDIO_SAMPLE_RATE, AUDIO_BOUNDARY_BITS_PER_SAMPLE, AUDIO_INTERNAL_BITS_PER_SAMPLE,
             AUDIO_FRAME_EFFECTIVE_MS, AUDIO_FRAME_TARGET_MS, AUDIO_FRAME_FALLBACK_ACTIVE ? 1 : 0,
             OPUS_BITRATE / 1000);
    ESP_LOGI(TAG, "======================================");

    // Initialize memory monitor early for pre-flight checks
    ESP_ERROR_CHECK(memory_monitor_init());

    portal_ota_confirm_running_image();

#ifdef CONFIG_USE_ES8388
    ESP_LOGI(TAG, "Audio input: ES8388 codec (LIN2/RIN2)");
    ESP_LOGI(TAG, "Audio output: ES8388 headphone (monitor)");
#else
    ESP_LOGI(TAG, "Audio input: ADC");
    ESP_LOGI(TAG, "Audio output: UDA1334 DAC");
#endif

    // Initialize control layer
    if (display_init() != ESP_OK) {
        ESP_LOGW(TAG, "Display init failed, continuing without display");
    }
    ESP_ERROR_CHECK(buttons_init());

    // Initialize audio sources (tone generator)
    ESP_ERROR_CHECK(tone_gen_init(status.tone_freq_hz));

#ifdef CONFIG_USE_ES8388
    // Initialize ES8388 with DAC enabled for headphone monitor
    if (es8388_audio_init(true) != ESP_OK) {
        ESP_LOGW(TAG, "ES8388 init failed — check wiring (SDA=%d, SCL=%d, addr=0x10)",
                 I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO);
        ESP_LOGW(TAG, "Continuing without audio");
    }
#else
    ESP_ERROR_CHECK(adc_audio_init());
    ESP_ERROR_CHECK(i2s_audio_init());
#endif

    // Create TX pipeline with Opus encoding BEFORE network init
    // Opus codec needs ~20KB heap - allocate before WiFi/mesh consumes heap
    ESP_LOGI(TAG, "Creating audio pipeline (heap: %lu bytes)...", 
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    
    // Pre-flight heap check before pipeline creation
    if (!memory_monitor_is_heap_healthy()) {
        ESP_LOGE(TAG, "Insufficient heap for audio pipeline! Free: %lu, Largest: %lu",
                 (unsigned long)memory_monitor_get_free_heap(),
                 (unsigned long)memory_monitor_get_largest_free_block());
        // Continue with warning - let pipeline creation fail gracefully if needed
    }
    
    adf_pipeline_config_t pipeline_cfg = ADF_PIPELINE_CONFIG_DEFAULT();
    pipeline_cfg.type = ADF_PIPELINE_TX;
    pipeline_cfg.enable_local_output = true;  // Enable headphone monitoring
    pipeline_cfg.opus_bitrate = OPUS_BITRATE;
    pipeline_cfg.opus_complexity = OPUS_COMPLEXITY;
    
    tx_pipeline = adf_pipeline_create(&pipeline_cfg);
    if (!tx_pipeline) {
        ESP_LOGE(TAG, "Failed to create TX pipeline");
        return;
    }
    
    ESP_LOGI(TAG, "Audio pipeline created (heap: %lu bytes remaining)",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));

    // Start TX pipeline before mesh init so the encode task can reserve its stack
    // while contiguous internal RAM is still available.
    ESP_ERROR_CHECK(adf_pipeline_start(tx_pipeline));
    status.audio_active = false;

    // Register audio pipeline tasks for stack monitoring
    // Task handles are internal to adf_pipeline, so we use the global getter
    adf_pipeline_handle_t p = tx_pipeline;
    if (p) {
        // TX pipeline has capture and encode tasks
        extern TaskHandle_t adf_pipeline_get_capture_task(adf_pipeline_handle_t);
        extern TaskHandle_t adf_pipeline_get_encode_task(adf_pipeline_handle_t);
        TaskHandle_t cap = adf_pipeline_get_capture_task(p);
        TaskHandle_t enc = adf_pipeline_get_encode_task(p);
        if (cap) memory_monitor_register_task(cap, "adf_cap");
        if (enc) memory_monitor_register_task(enc, "adf_enc");
    }

#if ENABLE_SRC_USB_PORTAL_NETWORK
    // Start portal before mesh allocates WiFi/mesh memory so portal can reserve
    // TinyUSB/HTTP resources without tripping low-heap startup guards.
    vTaskDelay(pdMS_TO_TICKS(PORTAL_INIT_SETTLE_MS));
    esp_err_t portal_err = portal_init();
    if (portal_err != ESP_OK) {
        ESP_LOGW(TAG, "Portal init failed: %s (continuing without portal)", esp_err_to_name(portal_err));
    }
#else
    ESP_LOGW(TAG, "USB portal networking disabled by config; serial monitor remains available");
#endif

    // Initialize network layer (ESP-WIFI-MESH) after pipeline tasks are allocated.
    // WiFi/mesh stack consumes significant heap (~40KB).
    ESP_LOGI(TAG, "Starting mesh network...");
    ESP_ERROR_CHECK(network_init_mesh());

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

    ESP_LOGI(TAG, "SRC initialized, waiting for network...");
    dashboard_init();

    // Wait for network to be ready (event-driven, not polling)
    // Use chunked waits to feed watchdog during mesh formation (can take 10+ seconds)
    ESP_ERROR_CHECK(network_register_startup_notification(xTaskGetCurrentTaskHandle()));
    uint32_t notify_value = 0;
    while (notify_value == 0) {
        notify_value = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));  // Wait 1 second at a time
        esp_task_wdt_reset();  // Feed watchdog while waiting
    }
    ESP_LOGI(TAG, "Network ready - starting audio pipeline");
    ESP_LOGI(TAG, "SRC STARTED - SRC_ID: %s, Root: %s",
             network_get_src_id(), network_is_root() ? "YES" : "NO");
    dashboard_log("Network ready");

    ESP_LOGI(TAG, "Main task stack high water mark: %u bytes", uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGI(TAG, "Free heap: %u bytes", heap_caps_get_free_size(MALLOC_CAP_8BIT));

    // Start periodic memory monitoring (logs every 10 seconds)
    memory_monitor_start_periodic(10000);

    // Main control loop - just handles UI, pipeline runs in separate tasks
    int64_t last_button_ms = esp_timer_get_time() / 1000;
    int64_t last_display_ms = last_button_ms;
    int64_t last_stats_ms = last_button_ms;
    uint32_t tx_obs_log_tick = 0;

    while (1) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(10));
        
        int64_t now_ms = esp_timer_get_time() / 1000;
        
        // Button polling every 5ms
        if (now_ms - last_button_ms >= 5) {
            last_button_ms = now_ms;
            button_event_t btn_event = buttons_poll();
            if (btn_event == BUTTON_EVENT_SHORT_PRESS) {
                current_view = (current_view + 1) % DISPLAY_VIEW_COUNT;
                dashboard_log("View: %d", current_view);
            } else if (btn_event == BUTTON_EVENT_LONG_PRESS) {
                status.input_mode = (status.input_mode + 1) % 3;
                adf_input_mode_t adf_mode = (status.input_mode == INPUT_MODE_TONE) ? ADF_INPUT_MODE_TONE :
                                            (status.input_mode == INPUT_MODE_USB) ? ADF_INPUT_MODE_USB :
                                            ADF_INPUT_MODE_AUX;
                adf_pipeline_set_input_mode(tx_pipeline, adf_mode);
                dashboard_log("Input: %d", status.input_mode);
            }
        }
        
        // Tone oscillation for test mode
        if (status.input_mode == INPUT_MODE_TONE) {
            update_tone_oscillate(now_ms);
        }

        // Stats update every 1000ms
        if (now_ms - last_stats_ms >= 1000) {
            status.connected_nodes = network_get_connected_nodes();
            status.nearest_rssi = network_get_nearest_child_rssi();
            
            // Get pipeline stats
            adf_pipeline_stats_t stats;
            if (adf_pipeline_get_stats(tx_pipeline, &stats) == ESP_OK) {
                update_status_from_pipeline_stats(&stats);
                // Bandwidth from actual bytes sent over mesh
                uint32_t tx_bytes = network_get_tx_bytes_and_reset();
                status.bandwidth_kbps = (tx_bytes * 8) / 1000;
                
                dashboard_log("TX: %lu frames, %lu nodes, %lukbps",
                             stats.frames_processed, status.connected_nodes, status.bandwidth_kbps);

                tx_obs_log_tick++;
                if ((tx_obs_log_tick % 5U) == 0U) {
                    network_transport_stats_t transport_stats = {0};
                    if (network_get_transport_stats(&transport_stats) == ESP_OK) {
                        dashboard_log(
                            "TX OBS: sent=%lu fail=%lu qfull=%lu bp=%lu, ctrl=%lu/%lu, churn(pc=%lu pd=%lu np=%lu rj=%lu/%lu/%lu)",
                            (unsigned long)transport_stats.tx_audio_packets,
                            (unsigned long)transport_stats.tx_audio_send_failures,
                            (unsigned long)transport_stats.tx_audio_queue_full,
                            (unsigned long)transport_stats.tx_audio_backpressure_level,
                            (unsigned long)transport_stats.tx_control_packets,
                            (unsigned long)transport_stats.tx_control_send_failures,
                            (unsigned long)transport_stats.parent_connect_events,
                            (unsigned long)transport_stats.parent_disconnect_events,
                            (unsigned long)transport_stats.no_parent_events,
                            (unsigned long)transport_stats.rejoin_trigger_events,
                            (unsigned long)transport_stats.rejoin_blocked_events,
                            (unsigned long)transport_stats.rejoin_circuit_breaker_events);
                    }
                }
            }
            
            dashboard_render_src(&status);
            last_stats_ms = now_ms;
        }

        // Display update every 100ms
        if (now_ms - last_display_ms >= 100) {
            last_display_ms = now_ms;
            adf_pipeline_stats_t stats;
            if (adf_pipeline_get_stats(tx_pipeline, &stats) == ESP_OK) {
                update_status_from_pipeline_stats(&stats);
            }
            display_render_src(current_view, &status);
        }
    }
}

#endif  // CONFIG_SRC_BUILD
