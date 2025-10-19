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

static const char *TAG = "rx_main";

static rx_status_t status = {
    .rssi = -100,
    .latency_ms = 0,
    .hops = 0,
    .receiving_audio = false,
    .bandwidth_kbps = 0
};

static display_view_t current_view = DISPLAY_VIEW_NETWORK;

void app_main(void) {
    ESP_LOGI(TAG, "MeshNet Audio RX starting...");
    
    // Initialize control layer
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(buttons_init());
    
    // Initialize network layer (STA mode)
    ESP_ERROR_CHECK(network_init_sta());
    
    // Initialize audio output
    ESP_ERROR_CHECK(i2s_audio_init());
    
    ESP_LOGI(TAG, "RX initialized, starting main loop");
    
    uint8_t packet_buffer[MAX_PACKET_SIZE];
    int16_t audio_frame[AUDIO_FRAME_SAMPLES];
    uint32_t last_packet_time = 0;
    uint32_t packets_received = 0;
    
    while (1) {
        // Handle button events
        button_event_t btn_event = buttons_poll();
        if (btn_event == BUTTON_EVENT_SHORT_PRESS) {
            current_view = (current_view == DISPLAY_VIEW_NETWORK) ? 
                          DISPLAY_VIEW_AUDIO : DISPLAY_VIEW_NETWORK;
            ESP_LOGI(TAG, "View changed to %s", 
                    current_view == DISPLAY_VIEW_NETWORK ? "Network" : "Audio");
        }
        
        // Receive audio packets
        size_t received_len;
        esp_err_t ret = network_udp_recv(packet_buffer, MAX_PACKET_SIZE, 
                                        &received_len, AUDIO_FRAME_MS);
        
        if (ret == ESP_OK && received_len == AUDIO_FRAME_BYTES) {
            memcpy(audio_frame, packet_buffer, AUDIO_FRAME_BYTES);
            i2s_audio_write_samples(audio_frame, AUDIO_FRAME_SAMPLES);
            
            status.receiving_audio = true;
            packets_received++;
            last_packet_time = xTaskGetTickCount();
        } else {
            // No packet received - check timeout
            if ((xTaskGetTickCount() - last_packet_time) > pdMS_TO_TICKS(100)) {
                status.receiving_audio = false;
            }
        }
        
        // Update network stats
        status.rssi = network_get_rssi();
        status.latency_ms = network_get_latency_ms();
        status.bandwidth_kbps = (packets_received * AUDIO_FRAME_BYTES * 8) / 
                               (xTaskGetTickCount() * portTICK_PERIOD_MS);
        
        // Update display
        display_render_rx(current_view, &status);
        
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
