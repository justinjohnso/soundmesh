#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include "config/build.h"
#include "config/pins.h"
#include "control/display.h"
#include "control/buttons.h"
#include "control/status.h"
#include "network/mesh_net.h"
#include <string.h>
#include "audio/i2s_audio.h"
// #include "audio/opus_codec.h"  // Removed for now
#include "audio/ring_buffer.h"
#include <netinet/in.h>

static const char *TAG = "rx_main";

static rx_status_t status = {
    .rssi = -100,
    .latency_ms = 0,
    .hops = 1,  // Direct connection
    .receiving_audio = false,
    .bandwidth_kbps = 0
};

static display_view_t current_view = DISPLAY_VIEW_NETWORK;
static ring_buffer_t *jitter_buffer = NULL;

#define JITTER_BUFFER_FRAMES 10  // 10 frames * 10ms = 100ms buffer
#define PREFILL_FRAMES 5         // Prefill 5 frames = 50ms latency

// Move large buffers to static storage to avoid stack overflow
static int16_t rx_audio_frame[AUDIO_FRAME_SAMPLES * 2];
static int16_t rx_silence_frame[AUDIO_FRAME_SAMPLES * 2] = {0};

// Packet tracking for statistics
static uint32_t packets_received = 0;
static uint32_t dropped_packets = 0;
static uint16_t last_seq = 0;
static bool first_packet = true;
static uint32_t last_packet_time = 0;

// Audio callback for mesh network - called when audio frames are received
static void audio_rx_callback(const uint8_t *payload, size_t len, uint16_t seq, uint32_t timestamp) {
    if (len != AUDIO_FRAME_BYTES) {
        ESP_LOGW(TAG, "Invalid audio frame size: %d", len);
        return;
    }
    
    // Track sequence gaps for packet loss measurement
    if (!first_packet) {
        uint16_t expected_seq = (last_seq + 1) & 0xFFFF;
        if (seq != expected_seq) {
            int16_t gap = (int16_t)(seq - expected_seq);
            if (gap > 0) {
                dropped_packets += gap;
            }
        }
    }
    first_packet = false;
    last_seq = seq;
    
    // Write to jitter buffer
    esp_err_t write_ret = ring_buffer_write(jitter_buffer, payload, AUDIO_FRAME_BYTES);
    if (write_ret == ESP_OK) {
        status.receiving_audio = true;
        packets_received++;
        last_packet_time = xTaskGetTickCount();
        
        if ((packets_received & 0x7F) == 0) {
            ESP_LOGI(TAG, "RX packet %lu (seq=%u)", packets_received, seq);
        }
    } else {
        ESP_LOGW(TAG, "Jitter buffer full, dropping packet seq=%u", seq);
    }
}

void app_main(void) {
ESP_LOGI(TAG, "MeshNet Audio RX starting...");

// Initialize control layer
if (display_init() != ESP_OK) {
ESP_LOGW(TAG, "Display init failed, continuing without display");
}
ESP_ERROR_CHECK(buttons_init());

// Initialize network layer (ESP-WIFI-MESH mode)
// Automatically joins existing mesh or becomes root if first
ESP_LOGI(TAG, "Starting mesh network...");
ESP_ERROR_CHECK(network_init_mesh());
ESP_ERROR_CHECK(network_start_latency_measurement());

// Register audio callback for mesh audio reception
ESP_ERROR_CHECK(network_register_audio_callback(audio_rx_callback));

// Initialize audio output
ESP_ERROR_CHECK(i2s_audio_init());
// ESP_ERROR_CHECK(opus_codec_init());  // Removed for now

// Create jitter buffer
jitter_buffer = ring_buffer_create(JITTER_BUFFER_FRAMES * AUDIO_FRAME_BYTES);
if (!jitter_buffer) {
ESP_LOGE(TAG, "Failed to create jitter buffer");
return;
}

ESP_LOGI(TAG, "RX initialized, registering for network startup notification");

// Wait for network to be stream-ready via event notification (not polling)
ESP_ERROR_CHECK(network_register_startup_notification(xTaskGetCurrentTaskHandle()));
uint32_t notify_value = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
if (notify_value > 0) {
    ESP_LOGI(TAG, "Network ready - starting audio reception");
}

// Log initial stack/heap status
ESP_LOGI(TAG, "Main task stack high water mark: %u bytes", uxTaskGetStackHighWaterMark(NULL));
ESP_LOGI(TAG, "Free heap: %u bytes", heap_caps_get_free_size(MALLOC_CAP_8BIT));

uint32_t bytes_received = 0;
uint32_t last_stats_update = xTaskGetTickCount();
uint32_t underrun_count = 0;
bool prefilled = false;
    
    while (1) {
        // Handle button events
        button_event_t btn_event = buttons_poll();
        if (btn_event == BUTTON_EVENT_SHORT_PRESS) {
            current_view = (current_view == DISPLAY_VIEW_NETWORK) ? 
                          DISPLAY_VIEW_AUDIO : DISPLAY_VIEW_NETWORK;
            ESP_LOGI(TAG, "View changed to %s", 
                    current_view == DISPLAY_VIEW_NETWORK ? "Network" : "Audio");
        }
        
        // Check for audio stream timeout (callback-based reception)
        if ((xTaskGetTickCount() - last_packet_time) > pdMS_TO_TICKS(100)) {
            status.receiving_audio = false;
            prefilled = false;
        }
        
        // Playback from jitter buffer with prefill
        size_t available_bytes = ring_buffer_available(jitter_buffer);
        size_t available_frames = available_bytes / AUDIO_FRAME_BYTES;
        if (!prefilled) {
            if (available_frames >= PREFILL_FRAMES) {
                prefilled = true;
                ESP_LOGI(TAG, "Buffer prefilled (%zu frames), starting playback", available_frames);
            }
        }
        
        if (prefilled) {
        if (ring_buffer_read(jitter_buffer, (uint8_t*)rx_audio_frame, AUDIO_FRAME_BYTES) == ESP_OK) {
        // Write 10ms frame in one go (I2S will buffer it)
        i2s_audio_write_samples(rx_audio_frame, AUDIO_FRAME_SAMPLES * 2);
        } else {
        // Buffer underrun - play silence (10ms worth)
        i2s_audio_write_samples(rx_silence_frame, AUDIO_FRAME_SAMPLES * 2);
        underrun_count++;
        if (underrun_count % 100 == 0) {
        ESP_LOGW(TAG, "Buffer underrun count: %lu", underrun_count);
        }
        }
        } else {
        // No audio stream - play silence to mute
        i2s_audio_write_samples(rx_silence_frame, AUDIO_FRAME_SAMPLES * 2);
        }
        
        // Update network stats every second
        uint32_t now = xTaskGetTickCount();
        if ((now - last_stats_update) >= pdMS_TO_TICKS(1000)) {
            uint32_t elapsed_ticks = now - last_stats_update;
            uint32_t elapsed_ms = elapsed_ticks * portTICK_PERIOD_MS;
            if (elapsed_ms > 0 && bytes_received > 0) {
                status.bandwidth_kbps = (bytes_received * 8) / elapsed_ms;
            }
            status.rssi = network_get_rssi();
            status.latency_ms = network_get_latency_ms();
            
            // Log packet loss statistics
            float loss_pct = 0.0f;
            if (packets_received + dropped_packets > 0) {
                loss_pct = (100.0f * dropped_packets) / (packets_received + dropped_packets);
            }
            ESP_LOGI(TAG, "Stats: RX=%lu pkts, DROP=%lu pkts, LOSS=%.1f%%, BW=%lu kbps", 
                     packets_received, dropped_packets, loss_pct, status.bandwidth_kbps);
            
            last_stats_update = now;
            bytes_received = 0;  // Reset for next interval
            // Don't reset packets_received/dropped_packets - keep cumulative for accurate loss %
        }
        
        // Update display at 10 Hz (every 100ms) to reduce I2C overhead
        static uint32_t last_display_update = 0;
        uint32_t now_display = xTaskGetTickCount();
        if ((now_display - last_display_update) >= pdMS_TO_TICKS(100)) {
            display_render_rx(current_view, &status);
            last_display_update = now_display;
        }
        
        // No delay - let I2S write timing control the loop (10ms per frame now)
    }
}
