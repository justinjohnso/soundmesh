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
#include "audio/ring_buffer.h"

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

#define JITTER_BUFFER_FRAMES 8
#define PREFILL_FRAMES 3

void app_main(void) {
    ESP_LOGI(TAG, "MeshNet Audio RX starting...");
    
    // Initialize control layer
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(buttons_init());
    
    // Initialize network layer (STA mode)
    ESP_ERROR_CHECK(network_init_sta());
    ESP_ERROR_CHECK(network_start_latency_measurement());

    // Initialize audio output
    ESP_ERROR_CHECK(i2s_audio_init());
    
    // Create jitter buffer
    jitter_buffer = ring_buffer_create(JITTER_BUFFER_FRAMES * AUDIO_FRAME_BYTES);
    if (!jitter_buffer) {
        ESP_LOGE(TAG, "Failed to create jitter buffer");
        return;
    }
    
    ESP_LOGI(TAG, "RX initialized, starting main loop");
    
    uint8_t packet_buffer[MAX_PACKET_SIZE];
    int16_t audio_frame[AUDIO_FRAME_SAMPLES];
    int16_t silence_frame[AUDIO_FRAME_SAMPLES] = {0};
    uint32_t last_packet_time = 0;
    uint32_t packets_received = 0;
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
        
        // Receive audio packets into jitter buffer
        size_t received_len;
        esp_err_t ret = network_udp_recv(packet_buffer, MAX_PACKET_SIZE, 
                                        &received_len, 1);
        
        if (ret == ESP_OK && received_len == AUDIO_FRAME_BYTES) {
            ring_buffer_write(jitter_buffer, packet_buffer, AUDIO_FRAME_BYTES);
            status.receiving_audio = true;
            packets_received++;
            bytes_received += received_len;
            last_packet_time = xTaskGetTickCount();
        } else {
            // No packet received - check timeout
            if ((xTaskGetTickCount() - last_packet_time) > pdMS_TO_TICKS(100)) {
                status.receiving_audio = false;
                prefilled = false;
            }
        }
        
        // Playback from jitter buffer with prefill
        size_t available_frames = ring_buffer_available(jitter_buffer);
        if (!prefilled) {
            if (available_frames >= PREFILL_FRAMES) {
                prefilled = true;
                ESP_LOGI(TAG, "Buffer prefilled, starting playback");
            }
        }
        
        if (prefilled) {
            if (ring_buffer_read(jitter_buffer, (uint8_t*)audio_frame, AUDIO_FRAME_BYTES) == ESP_OK) {
                ESP_LOGI(TAG, "Playing audio frame");
                i2s_audio_write_mono_as_stereo(audio_frame, AUDIO_FRAME_SAMPLES);
            } else {
                // Buffer underrun - play silence
                i2s_audio_write_mono_as_stereo(silence_frame, AUDIO_FRAME_SAMPLES);
                underrun_count++;
                if (underrun_count % 10 == 0) {
                    ESP_LOGW(TAG, "Buffer underrun count: %lu", underrun_count);
                }
            }
        } else {
            // No audio stream - play silence to mute
            i2s_audio_write_mono_as_stereo(silence_frame, AUDIO_FRAME_SAMPLES);
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
            last_stats_update = now;
            bytes_received = 0;  // Reset for next interval
        }
        
        // Update display
        display_render_rx(current_view, &status);
        
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
