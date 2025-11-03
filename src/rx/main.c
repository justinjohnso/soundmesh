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
static uint8_t rx_packet_buffer[MAX_PACKET_SIZE];
static int16_t rx_audio_frame[AUDIO_FRAME_SAMPLES * 2];
static int16_t rx_silence_frame[AUDIO_FRAME_SAMPLES * 2] = {0};

void app_main(void) {
ESP_LOGI(TAG, "MeshNet Audio RX starting...");

// Initialize control layer
if (display_init() != ESP_OK) {
ESP_LOGW(TAG, "Display init failed, continuing without display");
}
ESP_ERROR_CHECK(buttons_init());

// Initialize network layer (STA mode) - don't abort if connection fails
esp_err_t net_ret = network_init_sta();
if (net_ret != ESP_OK) {
    ESP_LOGW(TAG, "Failed to connect to TX AP, will retry in background");
    }
    // DISABLED: Latency task steals packets from audio socket
    // ESP_ERROR_CHECK(network_start_latency_measurement());

// Initialize audio output
ESP_ERROR_CHECK(i2s_audio_init());
// ESP_ERROR_CHECK(opus_codec_init());  // Removed for now

// Create jitter buffer
jitter_buffer = ring_buffer_create(JITTER_BUFFER_FRAMES * AUDIO_FRAME_BYTES);
if (!jitter_buffer) {
ESP_LOGE(TAG, "Failed to create jitter buffer");
return;
}

ESP_LOGI(TAG, "RX initialized, starting main loop");
    
// Log initial stack/heap status
ESP_LOGI(TAG, "Main task stack high water mark: %u bytes", uxTaskGetStackHighWaterMark(NULL));
ESP_LOGI(TAG, "Free heap: %u bytes", heap_caps_get_free_size(MALLOC_CAP_8BIT));

    uint32_t last_packet_time = 0;
    uint32_t packets_received = 0;
    uint32_t bytes_received = 0;
    uint32_t last_stats_update = xTaskGetTickCount();
    uint32_t underrun_count = 0;
    bool prefilled = false;
    uint16_t last_seq = 0;
    uint32_t dropped_packets = 0;
    bool first_packet = true;
    
    while (1) {
        // Handle button events
        button_event_t btn_event = buttons_poll();
        if (btn_event == BUTTON_EVENT_SHORT_PRESS) {
            current_view = (current_view == DISPLAY_VIEW_NETWORK) ? 
                          DISPLAY_VIEW_AUDIO : DISPLAY_VIEW_NETWORK;
            ESP_LOGI(TAG, "View changed to %s", 
                    current_view == DISPLAY_VIEW_NETWORK ? "Network" : "Audio");
        }
        
        // Receive raw PCM audio packets (10ms timeout matches frame period)
        size_t received_len;
        esp_err_t ret = network_udp_recv(rx_packet_buffer, MAX_PACKET_SIZE,
        &received_len, AUDIO_FRAME_MS);

        if (ret == ESP_OK) {
            // Parse minimal frame header
                if (received_len >= NET_FRAME_HEADER_SIZE) {
                    net_frame_header_t hdr;
                    memcpy(&hdr, rx_packet_buffer, NET_FRAME_HEADER_SIZE);
                    if (hdr.magic == NET_FRAME_MAGIC && hdr.version == NET_FRAME_VERSION && hdr.type == NET_PKT_TYPE_AUDIO_RAW) {
                        uint16_t payload_len = ntohs(hdr.payload_len);
                        uint16_t seq = ntohs(hdr.seq);
                        if (payload_len == AUDIO_FRAME_BYTES && received_len == (NET_FRAME_HEADER_SIZE + payload_len)) {
                            ESP_LOGD(TAG, "Received framed audio packet seq=%u payload=%u bytes", seq, payload_len);
                            
                            // Track sequence gaps to measure packet loss
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
                            
                            uint8_t *payload = rx_packet_buffer + NET_FRAME_HEADER_SIZE;
                            esp_err_t write_ret = ring_buffer_write(jitter_buffer, payload, AUDIO_FRAME_BYTES);
                            if (write_ret == ESP_OK) {
                                status.receiving_audio = true;
                                packets_received++;
                                bytes_received += payload_len;
                                last_packet_time = xTaskGetTickCount();
                                if ((packets_received & 0x7F) == 0) {
                                    ESP_LOGI(TAG, "RX packet %lu", packets_received);
                                }
                            } else {
                                ESP_LOGW(TAG, "Failed to write to jitter buffer: %s", esp_err_to_name(write_ret));
                            }
                        } else {
                            // Dump first 16 bytes for debugging header/payload alignment
                            char hexbuf[3 * 16 + 1];
                            int to_print = (received_len < 16) ? received_len : 16;
                            for (int i = 0; i < to_print; ++i) {
                                sprintf(&hexbuf[i * 3], "%02X ", (unsigned char)rx_packet_buffer[i]);
                            }
                            hexbuf[to_print * 3] = '\0';
                            ESP_LOGW(TAG, "Audio frame size mismatch: payload_len=%d received_len=%d seq=%u head=%s", payload_len, received_len, seq, hexbuf);
                        }
                    } else {
                        ESP_LOGW(TAG, "Invalid frame header: magic=0x%02x version=%d type=%d", hdr.magic, hdr.version, hdr.type);
                    }
            } else if (received_len == AUDIO_FRAME_BYTES) {
                // Legacy payload-only frame (no header). Accept for backward compatibility.
                    ESP_LOGD(TAG, "Received legacy raw audio packet (no header), payload=%d bytes", received_len);
                esp_err_t write_ret = ring_buffer_write(jitter_buffer, rx_packet_buffer, AUDIO_FRAME_BYTES);
                if (write_ret == ESP_OK) {
                    status.receiving_audio = true;
                    packets_received++;
                    bytes_received += received_len;
                    last_packet_time = xTaskGetTickCount();

                } else {
                    ESP_LOGW(TAG, "Failed to write legacy audio to jitter buffer: %s", esp_err_to_name(write_ret));
                }
            } else {
                ESP_LOGW(TAG, "Received too-small packet without header: len=%d", received_len);
            }
        } else if (ret == ESP_ERR_TIMEOUT) {
            // Normal timeout, no packet
        } else {
            ESP_LOGW(TAG, "Network receive error: %s", esp_err_to_name(ret));
            // No packet received - check timeout
            if ((xTaskGetTickCount() - last_packet_time) > pdMS_TO_TICKS(100)) {
                status.receiving_audio = false;
                prefilled = false;
            }
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
