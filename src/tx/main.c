#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_task_wdt.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <string.h>
#include <arpa/inet.h>
#include "config/build.h"
#include "config/pins.h"
#include "control/display.h"
#include "control/buttons.h"
#include "control/status.h"
#include "network/mesh_net.h"
#include "audio/tone_gen.h"
#include "audio/usb_audio.h"
#include "audio/adc_audio.h"
// #include "audio/opus_codec.h"  // Removed for now
#include "audio/ring_buffer.h"

static const char *TAG = "tx_main";

static tx_status_t status = {
.input_mode = INPUT_MODE_USB,
.audio_active = false,
.connected_nodes = 0,
.latency_ms = 10,
.bandwidth_kbps = 0,
.rssi = 0,
.tone_freq_hz = 110
};

static display_view_t current_view = DISPLAY_VIEW_NETWORK;
static ring_buffer_t *audio_buffer = NULL;
static adc_oneshot_unit_handle_t adc1_handle;
static adc_cali_handle_t adc1_cali_handle = NULL;

// Global audio buffers to reduce stack usage (oracle recommendation #1)
static int16_t mono_frame[AUDIO_FRAME_SAMPLES];
static int16_t stereo_frame[AUDIO_FRAME_SAMPLES * 2];
// TX uses a temporary buffer for framed packets (header + payload)
static uint8_t packet_buffer[AUDIO_FRAME_BYTES];
static uint8_t framed_buffer[NET_FRAME_HEADER_SIZE + AUDIO_FRAME_BYTES];
static uint16_t tx_seq = 0;

// ADC processing function (oracle recommendation #2: simplify main loop)
void update_tone_from_adc(void) {
    if (adc1_handle == NULL || adc1_cali_handle == NULL) {
        return; // ADC not initialized
    }

    int adc_raw;
    if (adc_oneshot_read(adc1_handle, ADC_CHANNEL_2, &adc_raw) != ESP_OK) {
        return;
    }

    int voltage_mv;
    if (adc_cali_raw_to_voltage(adc1_cali_handle, adc_raw, &voltage_mv) != ESP_OK) {
        return;
    }

    // Map voltage (0-3300mV) to frequency (200-2000Hz)
    uint32_t new_freq = 200 + ((voltage_mv * 1800) / 3300);
    if (new_freq != status.tone_freq_hz) {
        status.tone_freq_hz = new_freq;
        tone_gen_set_frequency(status.tone_freq_hz);
        ESP_LOGI(TAG, "Tone frequency updated to %lu Hz", status.tone_freq_hz);
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "MeshNet Audio TX starting...");
    
    // Initialize control layer
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(buttons_init());
    
    // Initialize network layer (AP mode)
    ESP_ERROR_CHECK(network_init_ap());
    ESP_ERROR_CHECK(network_start_latency_measurement());

    // Initialize ADC for pitch control (GPIO 2 - ADC1_CHANNEL_2)
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_2, &config));

    // Initialize ADC calibration
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .chan = ADC_CHANNEL_2,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cali_config, &adc1_cali_handle));

    // Initialize audio layer
    ESP_ERROR_CHECK(tone_gen_init(status.tone_freq_hz));
    ESP_ERROR_CHECK(usb_audio_init());
    ESP_ERROR_CHECK(adc_audio_init());
    // ESP_ERROR_CHECK(opus_codec_init());  // Removed for now

    // Create ring buffer
    audio_buffer = ring_buffer_create(RING_BUFFER_SIZE);
    if (!audio_buffer) {
        ESP_LOGE(TAG, "Failed to create ring buffer");
        return;
    }
    
    // Initialize watchdog timer (oracle recommendation #4)
    // Check if already initialized (ESP-IDF might have done it)
    esp_err_t wdt_err = esp_task_wdt_init(&(esp_task_wdt_config_t){
        .timeout_ms = 5000,  // 5 second timeout
        .idle_core_mask = 0,
        .trigger_panic = true,
    });

    if (wdt_err == ESP_ERR_INVALID_STATE) {
        // Already initialized, just add our task
        ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    } else {
        ESP_ERROR_CHECK(wdt_err);
        ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    }

    ESP_LOGI(TAG, "TX initialized, starting main loop");

    uint32_t bytes_sent = 0;
    uint32_t last_stats_update = xTaskGetTickCount();
    
    while (1) {
        // Handle button events
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
        
        // Generate/capture audio based on mode
        status.audio_active = false;
        switch (status.input_mode) {
        case INPUT_MODE_TONE:
            update_tone_from_adc();
            tone_gen_fill_buffer(mono_frame, AUDIO_FRAME_SAMPLES);
            // Convert mono to stereo (duplicate channels)
            for (size_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
                stereo_frame[i * 2] = stereo_frame[i * 2 + 1] = mono_frame[i];
            }
            status.audio_active = true;
            break;
        case INPUT_MODE_USB:
            if (usb_audio_is_active()) {
                size_t frames_read;
                usb_audio_read_frames(stereo_frame, AUDIO_FRAME_SAMPLES, &frames_read);
                status.audio_active = true;
            }
            break;
        case INPUT_MODE_AUX:
            {
                uint8_t left_val, right_val;
                if (adc_audio_read(&left_val, &right_val) == ESP_OK) {
                    // Monitor ADC values for debugging
                    ESP_LOGI(TAG, "ADC L: %d, R: %d", left_val, right_val);

                    // Check for audio signal - transmit any non-zero ADC values for maximum sensitivity
                    const uint8_t AUDIO_THRESHOLD = 0; // Minimum ADC value to detect audio (any signal above silence)

                    if (left_val > AUDIO_THRESHOLD || right_val > AUDIO_THRESHOLD) {
                    // Convert 8-bit ADC to 16-bit PCM
                    // Note: Your signal range is very limited (0-30). For better audio quality, consider:
                    // 1. Adding DC bias around 1.65V (VCC/2) so signal can swing +/- from center
                    // 2. AC coupling capacitor to remove DC offset
                    // 3. Audio preamplifier for higher signal level

                    // Current scaling: map 0-30 range to full 16-bit range
                    const float ADC_MAX_OBSERVED = 30.0f;  // Your observed max value
                    const float SCALE_FACTOR = 65535.0f / ADC_MAX_OBSERVED;  // Scale to full 16-bit range

                    for (size_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
                        stereo_frame[i * 2] = (int16_t)(left_val * SCALE_FACTOR);
                    stereo_frame[i * 2 + 1] = (int16_t)(right_val * SCALE_FACTOR);
                        }
                        status.audio_active = true;
                        ESP_LOGI(TAG, "Stereo audio detected (L: %d->%d, R: %d->%d)",
                                left_val, stereo_frame[0], right_val, stereo_frame[1]);
                    } else {
                        // No audio detected, fill with silence
                        memset(stereo_frame, 0, sizeof(stereo_frame));
                        status.audio_active = false;
                    }
                }
            }
            break;
        default:
            // Handle any unhandled cases
            break;
        }
        
        // If we have audio, send raw PCM
        if (status.audio_active) {
        memcpy(packet_buffer, stereo_frame, AUDIO_FRAME_BYTES);

        // Build frame header
        net_frame_header_t hdr;
        hdr.magic = NET_FRAME_MAGIC;
        hdr.version = NET_FRAME_VERSION;
        hdr.type = NET_PKT_TYPE_AUDIO_RAW;
        hdr.reserved = 0;
        hdr.seq = htons(tx_seq++);
        hdr.timestamp = htonl((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
        hdr.payload_len = htons((uint16_t)AUDIO_FRAME_BYTES);

        // Copy header + payload to framed buffer
        memcpy(framed_buffer, &hdr, sizeof(hdr));
        memcpy(framed_buffer + sizeof(hdr), packet_buffer, AUDIO_FRAME_BYTES);

        esp_err_t send_ret = network_udp_send(framed_buffer, sizeof(hdr) + AUDIO_FRAME_BYTES);
        if (send_ret == ESP_OK) {
        bytes_sent += (NET_FRAME_HEADER_SIZE + AUDIO_FRAME_BYTES);
        ESP_LOGI(TAG, "Sent framed audio packet seq=%u header+payload=%d bytes payload=%d bytes", ntohs(hdr.seq), NET_FRAME_HEADER_SIZE + AUDIO_FRAME_BYTES, AUDIO_FRAME_BYTES);
        } else {
        ESP_LOGW(TAG, "Failed to send audio packet: %s", esp_err_to_name(send_ret));
        }
        }

        // Note: ping processing and responses are handled inside the network layer.
        // Avoid application-level ping rebroadcast to prevent ping storms.

        // Update network stats every second
        uint32_t now = xTaskGetTickCount();
        if ((now - last_stats_update) >= pdMS_TO_TICKS(1000)) {
            uint32_t elapsed_ticks = now - last_stats_update;
            uint32_t elapsed_ms = elapsed_ticks * portTICK_PERIOD_MS;
            if (elapsed_ms > 0 && bytes_sent > 0) {
                status.bandwidth_kbps = (bytes_sent * 8) / elapsed_ms;
            }
            status.connected_nodes = network_get_connected_nodes();
            status.rssi = network_get_rssi();
            status.latency_ms = network_get_latency_ms();
            last_stats_update = now;
            bytes_sent = 0;  // Reset for next interval
        }

        // Update display
        display_render_tx(current_view, &status);

        // Reset watchdog
        esp_task_wdt_reset();

        vTaskDelay(pdMS_TO_TICKS(AUDIO_FRAME_MS));
    }
}
