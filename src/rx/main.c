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
#include "audio/adf_pipeline.h"

#ifdef CONFIG_USE_ES8388
#include "audio/es8388_audio.h"
#else
#include "audio/i2s_audio.h"
#endif
#include <string.h>
#include <netinet/in.h>
#include <esp_timer.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include "control/serial_dashboard.h"
#include "control/usb_portal.h"

static const char *TAG = "rx_main";

static adc_oneshot_unit_handle_t adc_handle = NULL;
static adc_cali_handle_t adc_cali_handle = NULL;
static int32_t battery_mv_ewma = 0;  // EWMA-smoothed voltage in mV

static uint8_t battery_read_percent(void) {
    if (!adc_handle) return 0;
    
    int raw = 0, voltage_mv = 0, sum = 0;
    for (int i = 0; i < 16; i++) {
        adc_oneshot_read(adc_handle, BATTERY_ADC_CHANNEL, &raw);
        if (adc_cali_handle) {
            adc_cali_raw_to_voltage(adc_cali_handle, raw, &voltage_mv);
            sum += voltage_mv;
        } else {
            sum += raw;
        }
    }
    int32_t sample_mv = (sum / 16) * BATTERY_DIVIDER_RATIO;
    
    // Heavy EWMA smoothing: alpha ≈ 1/16, settles over ~15 seconds
    if (battery_mv_ewma == 0) {
        battery_mv_ewma = sample_mv;  // seed on first read
    } else {
        battery_mv_ewma = (battery_mv_ewma * 15 + sample_mv) / 16;
    }
    
    if (battery_mv_ewma >= BATTERY_VOLTAGE_FULL_MV) return 100;
    if (battery_mv_ewma <= BATTERY_VOLTAGE_EMPTY_MV) return 0;
    return (uint8_t)((battery_mv_ewma - BATTERY_VOLTAGE_EMPTY_MV) * 100 
                     / (BATTERY_VOLTAGE_FULL_MV - BATTERY_VOLTAGE_EMPTY_MV));
}

static rx_status_t status = {
    .rssi = -100,
    .latency_ms = 0,
    .buffer_pct = 0,
    .receiving_audio = false,
    .bandwidth_kbps = 0
};

// Bandwidth tracking (per-second rate, not cumulative)
static uint32_t bytes_received_this_second = 0;

static display_view_t current_view = DISPLAY_VIEW_AUDIO;
static adf_pipeline_handle_t rx_pipeline = NULL;

typedef enum {
    RX_STATE_INIT = 0,
    RX_STATE_MESH_JOINING,
    RX_STATE_MESH_READY,
    RX_STATE_WAITING_FOR_STREAM,
    RX_STATE_STREAM_FOUND,
    RX_STATE_STREAMING,
    RX_STATE_STREAM_LOST,
    RX_STATE_ERROR_NO_MESH
} rx_connection_state_t;

static rx_connection_state_t current_state = RX_STATE_INIT;
static char receiving_from_src_id[NETWORK_SRC_ID_LEN] = {0};
static int64_t state_change_time_ms = 0;
static int64_t last_waiting_log_ms = 0;

// Packet tracking for statistics
static uint32_t packets_received = 0;
static uint32_t dropped_packets = 0;
static uint16_t last_seq = 0;
static bool first_packet = true;
static uint32_t last_packet_time = 0;
static uint32_t stream_silence_confirm_start = 0;
static int64_t last_stream_rx_ms = 0;
static int64_t last_rejoin_attempt_ms = 0;

// One-way latency estimation from audio frame timestamps
// Root puts esp_timer ms in each frame header; we compare to our local clock.
// Since clocks aren't synced, we track min(local - remote) over a sliding
// window.  The minimum raw delta ≈ clock_offset + true one-way latency.
// We report delta - min(delta) as the jitter-adjusted latency estimate,
// and periodically reset min to adapt to clock drift.
static int64_t min_raw_delta = INT64_MAX;   // Minimum (local_ms - remote_ms) seen in current window
static uint32_t ewma_oneway_ms = 0;         // Smoothed latency estimate
static uint32_t latency_window_start = 0;   // Tick count when current window started
#define LATENCY_WINDOW_MS 10000             // Reset min every 10s to track drift

// Audio callback for mesh network - called when audio frames are received
static const char *rx_state_to_string(rx_connection_state_t state) {
    switch (state) {
        case RX_STATE_INIT: return "Init";
        case RX_STATE_MESH_JOINING: return "Mesh Joining";
        case RX_STATE_MESH_READY: return "Mesh Ready";
        case RX_STATE_WAITING_FOR_STREAM: return "Waiting";
        case RX_STATE_STREAM_FOUND: return "Stream Found";
        case RX_STATE_STREAMING: return "Streaming";
        case RX_STATE_STREAM_LOST: return "Stream Lost";
        case RX_STATE_ERROR_NO_MESH: return "No Mesh";
        default: return "Unknown";
    }
}

static void rx_set_connection_state(rx_connection_state_t next_state, const char *reason) {
    if (current_state == next_state) {
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    ESP_LOGI(TAG, "State: %s -> %s (%s)",
             rx_state_to_string(current_state),
             rx_state_to_string(next_state),
             reason ? reason : "no reason");
    current_state = next_state;
    state_change_time_ms = now_ms;
}

static void update_rx_status_state_fields(int64_t now_ms) {
    strlcpy(status.connection_state, rx_state_to_string(current_state), sizeof(status.connection_state));
    status.state_elapsed_s = (state_change_time_ms > 0 && now_ms >= state_change_time_ms)
                                 ? (uint32_t)((now_ms - state_change_time_ms) / 1000)
                                 : 0;
    if (receiving_from_src_id[0]) {
        strlcpy(status.source_src_id, receiving_from_src_id, sizeof(status.source_src_id));
    } else {
        status.source_src_id[0] = '\0';
    }
}

static void audio_rx_callback(const uint8_t *payload, size_t len, uint16_t seq, uint32_t timestamp, const char *src_id) {
    // Track sequence gaps for packet loss measurement.
    // Callback is invoked per unpacked Opus frame (seq, seq+1, ...), so expected step is +1.
    if (!first_packet) {
        uint16_t expected_seq = (last_seq + 1) & 0xFFFF;
        if (seq != expected_seq) {
            int16_t gap = (int16_t)(seq - expected_seq);
            if (gap > 0 && gap < 100) {
                dropped_packets += (uint32_t)gap;
            }
        }
    }
    first_packet = false;
    last_seq = seq;
    
    // One-way latency estimation from root's timestamp
    // raw_delta = local_ms - remote_ms (includes unknown clock offset + true latency)
    // min(raw_delta) over a window approximates the clock offset + best-case latency
    // Reported value = raw_delta - min(raw_delta) ≈ network jitter above baseline
    if (timestamp > 0) {
        int64_t local_ms = (int64_t)(esp_timer_get_time() / 1000);
        int64_t raw_delta = local_ms - (int64_t)timestamp;
        
        // Reset window periodically to adapt to clock drift
        uint32_t now_ticks = xTaskGetTickCount();
        if ((now_ticks - latency_window_start) > pdMS_TO_TICKS(LATENCY_WINDOW_MS)) {
            min_raw_delta = raw_delta;
            latency_window_start = now_ticks;
        }
        
        if (raw_delta < min_raw_delta) {
            min_raw_delta = raw_delta;
        }
        
        uint32_t oneway = (uint32_t)(raw_delta - min_raw_delta);
        ewma_oneway_ms = (ewma_oneway_ms * 9 + oneway) / 10;
    }

    // Feed Opus data to the pipeline
    if (rx_pipeline) {
        esp_err_t ret = adf_pipeline_feed_opus(rx_pipeline, payload, len, seq, timestamp);
        if (ret == ESP_OK) {
            last_stream_rx_ms = esp_timer_get_time() / 1000;
            bool was_receiving = status.receiving_audio;
            status.receiving_audio = true;
            packets_received++;
            bytes_received_this_second += len;
            last_packet_time = xTaskGetTickCount();

            if (src_id && src_id[0] != '\0' &&
                strncmp(receiving_from_src_id, src_id, sizeof(receiving_from_src_id)) != 0) {
                strlcpy(receiving_from_src_id, src_id, sizeof(receiving_from_src_id));
                ESP_LOGI(TAG, "Receiving stream from %s", receiving_from_src_id);
            }

            if (!was_receiving ||
                current_state == RX_STATE_WAITING_FOR_STREAM ||
                current_state == RX_STATE_STREAM_LOST ||
                current_state == RX_STATE_MESH_READY) {
                rx_set_connection_state(RX_STATE_STREAM_FOUND, "first audio frame");
                rx_set_connection_state(RX_STATE_STREAMING, "audio flow active");
            }
            
            if ((packets_received & 0x7F) == 0) {
                ESP_LOGI(TAG, "RX packet %lu (seq=%u, len=%zu, lat=%lums)", 
                         packets_received, seq, len, ewma_oneway_ms);
            }
        } else {
            ESP_LOGW(TAG, "Pipeline feed failed seq=%u err=%s", seq, esp_err_to_name(ret));
        }
    }
}

void app_main(void) {
    esp_log_level_set("*", ESP_LOG_INFO);
    ESP_LOGI(TAG, "======================================");
    ESP_LOGI(TAG, "*** APP_MAIN STARTED - RX beginning initialization ***");
    ESP_LOGI(TAG, "MeshNet Audio RX starting (Opus)...");
    ESP_LOGI(TAG, "Build: " __DATE__ " " __TIME__);
    ESP_LOGI(TAG, "Audio: %dHz, %d-bit, %dms frames",
             AUDIO_SAMPLE_RATE, AUDIO_BITS_PER_SAMPLE, AUDIO_FRAME_MS);
    ESP_LOGI(TAG, "======================================");
    state_change_time_ms = esp_timer_get_time() / 1000;
    last_waiting_log_ms = state_change_time_ms;
    rx_set_connection_state(RX_STATE_INIT, "boot");

    if (display_init() != ESP_OK) {
        ESP_LOGW(TAG, "Display init failed, continuing without display");
    }
    ESP_ERROR_CHECK(buttons_init());

    // Initialize battery ADC
    adc_oneshot_unit_init_cfg_t adc_cfg = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&adc_cfg, &adc_handle) == ESP_OK) {
        adc_oneshot_chan_cfg_t chan_cfg = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        adc_oneshot_config_channel(adc_handle, BATTERY_ADC_CHANNEL, &chan_cfg);
        
        adc_cali_curve_fitting_config_t cali_cfg = {
            .unit_id = ADC_UNIT_1,
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &adc_cali_handle) != ESP_OK) {
            ESP_LOGW(TAG, "ADC calibration not available, battery readings may be inaccurate");
            adc_cali_handle = NULL;
        }
        ESP_LOGI(TAG, "Battery ADC initialized on GPIO%d", BATTERY_ADC_GPIO);
    } else {
        ESP_LOGW(TAG, "Battery ADC init failed, battery monitoring disabled");
    }

    // Initialize audio output
#ifdef CONFIG_USE_ES8388
    ESP_LOGI(TAG, "Audio output: ES8388 headphone DAC");
    if (es8388_audio_init(true) != ESP_OK) {
        ESP_LOGW(TAG, "ES8388 init failed — check wiring (SDA=%d, SCL=%d, addr=0x10)", 
                 I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO);
        ESP_LOGW(TAG, "Continuing without audio output");
    }
#else
    ESP_LOGI(TAG, "Audio output: UDA1334 I2S DAC");
    ESP_ERROR_CHECK(i2s_audio_init());
#endif

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
    rx_set_connection_state(RX_STATE_MESH_JOINING, "starting mesh");
    ESP_ERROR_CHECK(network_init_mesh());

#if ENABLE_USB_PORTAL_NETWORK
    // RX/OUT portal runs over USB NCM so telemetry remains available without USB serial.
    esp_err_t portal_err = portal_init();
    if (portal_err != ESP_OK) {
        ESP_LOGW(TAG, "Portal init failed: %s (continuing)", esp_err_to_name(portal_err));
    }
#endif

    // Register audio callback for mesh audio reception
    ESP_ERROR_CHECK(network_register_audio_callback(audio_rx_callback));

    ESP_LOGI(TAG, "RX initialized, waiting for network...");
    dashboard_init();

    // Start the RX pipeline BEFORE waiting for network (for tone test / immediate audio)
    ESP_LOGI(TAG, "Starting audio pipeline...");
    ESP_ERROR_CHECK(adf_pipeline_start(rx_pipeline));

    // Register for network ready notification (non-blocking — we poll in the main loop)
    ESP_ERROR_CHECK(network_register_startup_notification(xTaskGetCurrentTaskHandle()));
    bool network_ready = false;

    ESP_LOGI(TAG, "Entering main loop, waiting for network...");

    uint32_t last_stats_update = xTaskGetTickCount();
    
    while (1) {
        // Handle button events (always, even before network is ready)
        button_event_t btn_event = buttons_poll();
        if (btn_event == BUTTON_EVENT_SHORT_PRESS) {
            current_view = (current_view + 1) % DISPLAY_VIEW_COUNT;
            dashboard_log("View: %d", current_view);
        }

        // Check for network ready notification (non-blocking)
        if (!network_ready) {
            uint32_t notify_value = ulTaskNotifyTake(pdTRUE, 0);
            if (notify_value > 0) {
                network_ready = true;
                ESP_LOGI(TAG, "Network ready");
                rx_set_connection_state(RX_STATE_MESH_READY, "mesh parent connected");
                rx_set_connection_state(RX_STATE_WAITING_FOR_STREAM, "waiting for first stream");
                dashboard_log("Network ready");
                ESP_LOGI(TAG, "Main task stack high water mark: %u bytes", uxTaskGetStackHighWaterMark(NULL));
                ESP_LOGI(TAG, "Free heap: %u bytes", heap_caps_get_free_size(MALLOC_CAP_8BIT));
            }
        }
        
        // Check for audio stream timeout
        if ((xTaskGetTickCount() - last_packet_time) > pdMS_TO_TICKS(STREAM_SILENCE_TIMEOUT_MS)) {
            if (status.receiving_audio) {
                uint32_t now_ticks = xTaskGetTickCount();
                if (stream_silence_confirm_start == 0) {
                    stream_silence_confirm_start = now_ticks;
                }
                if ((now_ticks - stream_silence_confirm_start) > pdMS_TO_TICKS(STREAM_SILENCE_CONFIRM_MS)) {
                    status.receiving_audio = false;
                    if (current_state == RX_STATE_STREAMING || current_state == RX_STATE_STREAM_FOUND) {
                        rx_set_connection_state(RX_STATE_STREAM_LOST, "stream silence timeout");
                        receiving_from_src_id[0] = '\0';
                        rx_set_connection_state(RX_STATE_WAITING_FOR_STREAM, "waiting for stream recovery");
                    }
                }
            }
        } else {
            stream_silence_confirm_start = 0;
        }

        bool mesh_connected = network_is_connected();
        if (!mesh_connected) {
            if (current_state != RX_STATE_INIT &&
                current_state != RX_STATE_MESH_JOINING &&
                current_state != RX_STATE_ERROR_NO_MESH) {
                receiving_from_src_id[0] = '\0';
                rx_set_connection_state(RX_STATE_ERROR_NO_MESH, "mesh disconnected");
            }
        } else if (current_state == RX_STATE_MESH_JOINING || current_state == RX_STATE_ERROR_NO_MESH) {
            rx_set_connection_state(RX_STATE_MESH_READY, "mesh connected");
            if (!status.receiving_audio) {
                rx_set_connection_state(RX_STATE_WAITING_FOR_STREAM, "mesh ready, waiting for stream");
            }
        }
        
        // Update network stats every second
        uint32_t now = xTaskGetTickCount();
        if (network_ready && (now - last_stats_update) >= pdMS_TO_TICKS(1000)) {
            status.rssi = network_get_rssi();
            status.latency_ms = ewma_oneway_ms;
            // Temporarily disable active RX->root pings during stability debugging.
            // They are non-essential for audio transport and currently generate frequent
            // ESP_ERR_MESH_ARGUMENT noise on degraded links, obscuring root-cause signals.
            
            // Calculate bandwidth rate (bytes this second * 8 / 1000 = kbps)
            status.bandwidth_kbps = (bytes_received_this_second * 8) / 1000;
            bytes_received_this_second = 0;
            
            status.battery_pct = battery_read_percent();
            
            // Get pipeline stats for buffer fill
            adf_pipeline_stats_t stats;
            if (adf_pipeline_get_stats(rx_pipeline, &stats) == ESP_OK) {
                status.buffer_pct = stats.buffer_fill_percent;
                
                float loss_pct = 0.0f;
                if (packets_received + dropped_packets > 0) {
                    loss_pct = (100.0f * dropped_packets) / (packets_received + dropped_packets);
                }
                status.loss_pct = loss_pct;
                dashboard_log("RX: %lu pkts, %lu drops (%.1f%%), buf=%u%%",
                             packets_received, dropped_packets, loss_pct, stats.buffer_fill_percent);
            }
            
            dashboard_render_rx(&status);
            last_stats_update = now;
        }
        
        int64_t now_ms = esp_timer_get_time() / 1000;
        update_rx_status_state_fields(now_ms);

        if (!status.receiving_audio && (now_ms - last_waiting_log_ms) >= 5000) {
            ESP_LOGI(TAG, "Still waiting for stream (State: %s, elapsed: %lus)",
                     rx_state_to_string(current_state), (unsigned long)status.state_elapsed_s);
            last_waiting_log_ms = now_ms;
        }

        // Self-heal: if we're mesh-connected but stream-starved for a sustained period,
        // force a mesh reconnect to refresh parent path selection.
        if (network_ready &&
            network_is_connected() &&
            !status.receiving_audio &&
            current_state == RX_STATE_WAITING_FOR_STREAM &&
            status.state_elapsed_s >= 60 &&
            (now_ms - last_stream_rx_ms) >= 60000 &&
            (now_ms - last_rejoin_attempt_ms) >= 180000) {
            ESP_LOGW(TAG, "No stream for %lus while connected, triggering mesh rejoin",
                     (unsigned long)status.state_elapsed_s);
            if (network_trigger_rejoin() == ESP_OK) {
                last_rejoin_attempt_ms = now_ms;
                rx_set_connection_state(RX_STATE_MESH_JOINING, "rejoin after prolonged stream starvation");
                receiving_from_src_id[0] = '\0';
            }
        }

        static uint32_t last_display_update = 0;
        if ((now - last_display_update) >= pdMS_TO_TICKS(100)) {
            display_render_rx(current_view, &status);
            last_display_update = now;
        }
        
        // Small delay to prevent tight loop
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
