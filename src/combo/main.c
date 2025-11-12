#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_log.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <esp_mesh.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <string.h>
#include <arpa/inet.h>
#include <math.h>
#include "config/build.h"
#include "config/pins.h"
#include "control/display.h"
#include "control/buttons.h"
#include "control/status.h"
#include "audio/tone_gen.h"
#include "audio/usb_audio.h"
#include "audio/adc_audio.h"
#include "audio/i2s_audio.h"  // Added for UDA1334 output
#include "audio/ring_buffer.h"
#include "network/mesh_net.h"

static const char *TAG = "combo_main";

// Compile-time check for v0.1 audio format
_Static_assert(AUDIO_BITS_PER_SAMPLE == 24 && AUDIO_CHANNELS == 1, "v0.1 requires 24-bit mono");

// Audio format conversion helpers: 16-bit → 24-bit packed (S24LE)
static inline void s24le_pack(int32_t s24, uint8_t* out) {
    out[0] = (uint8_t)(s24 & 0xFF);
    out[1] = (uint8_t)((s24 >> 8) & 0xFF);
    out[2] = (uint8_t)((s24 >> 16) & 0xFF);
}

static void pcm16_mono_to_pcm24_mono_pack(const int16_t* in, size_t frames, uint8_t* out) {
    for (size_t i = 0; i < frames; i++) {
        int32_t s24 = ((int32_t)in[i]) << 8;  // 16→24 bit zero-pad LSBs
        s24le_pack(s24, &out[i * 3]);
    }
}

static void pcm16_stereo_to_pcm24_mono_pack(const int16_t* in_lr, size_t frames, uint8_t* out) {
    for (size_t i = 0; i < frames; i++) {
        // Mono = average L+R (simple downmix)
        int32_t m = ((int32_t)in_lr[i*2] + (int32_t)in_lr[i*2 + 1]) >> 1;
        int32_t s24 = m << 8;
        s24le_pack(s24, &out[i * 3]);
    }
}

// Timer for 1ms pacing
static SemaphoreHandle_t combo_timer_sem = NULL;
static uint32_t ms_tick = 0;

static void combo_timer_callback(void* arg) {
    xSemaphoreGive(combo_timer_sem);
}

static combo_status_t status = {
.input_mode = INPUT_MODE_TONE,
.audio_active = false,
.connected_nodes = 0,
.latency_ms = 10,
.bandwidth_kbps = 0,
.rssi = 0,
.tone_freq_hz = 110,
.output_volume = 1.0f
};

static display_view_t current_view = DISPLAY_VIEW_AUDIO;  // Default to audio view
static ring_buffer_t *audio_buffer = NULL;

// Global audio buffers to reduce stack usage
static int16_t mono_frame[AUDIO_FRAME_SAMPLES];
static int16_t stereo_frame[AUDIO_FRAME_SAMPLES * 2];
static uint8_t packet_buffer[AUDIO_FRAME_BYTES];  // 24-bit packed mono for network
static uint8_t framed_buffer[NET_FRAME_HEADER_SIZE + AUDIO_FRAME_BYTES];

// Auto-oscillate tone frequency between 300-700 Hz
void update_tone_oscillate(void) {
    static uint32_t last_log = 0;
    
    // Oscillate over 4 seconds (200ms per frame = 20 frames = 4s for full sweep)
    uint32_t phase = (ms_tick / 20) % 200;  // 0-199 over 4 seconds
    float ratio = (float)phase / 200.0f;  // 0.0 to 1.0
    
    // Use sine wave for smooth oscillation
    float sine_val = sinf(ratio * 2.0f * M_PI);  // -1 to 1
    uint32_t center = 500;  // Midpoint between 300-700
    uint32_t range = 200;   // ±200 from center
    uint32_t new_freq = center + (uint32_t)(sine_val * range);
    
    // Only update if frequency changed significantly
    if (abs((int)new_freq - (int)status.tone_freq_hz) > 5) {
        status.tone_freq_hz = new_freq;
        tone_gen_set_frequency(status.tone_freq_hz);
    }
    
    // Log periodically
    uint32_t now = xTaskGetTickCount();
    if ((now - last_log) > pdMS_TO_TICKS(2000)) {
        ESP_LOGI(TAG, "Tone oscillating: freq=%lu Hz", status.tone_freq_hz);
        last_log = now;
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "MeshNet Audio COMBO starting...");

    // Initialize control layer
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(buttons_init());

    // Note: ADC not initialized for combo (no potentiometer attached)

    // Initialize network layer (ESP-WIFI-MESH mode)
    // Automatically forms mesh or joins existing mesh
    ESP_ERROR_CHECK(network_init_mesh());
    ESP_ERROR_CHECK(network_start_latency_measurement());

    // Initialize audio layer
    ESP_ERROR_CHECK(tone_gen_init(status.tone_freq_hz));
    ESP_ERROR_CHECK(usb_audio_init());
    ESP_ERROR_CHECK(adc_audio_init());
    ESP_ERROR_CHECK(i2s_audio_init());  // Initialize I2S output for UDA1334
    // Don't start ADC yet - will start when switching to AUX mode

    // Create ring buffer (not used for network, but keeping for consistency)
    audio_buffer = ring_buffer_create(RING_BUFFER_SIZE);
    if (!audio_buffer) {
        ESP_LOGE(TAG, "Failed to create ring buffer");
        return;
    }

    // Initialize watchdog timer
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

    // Create semaphore for timer
    combo_timer_sem = xSemaphoreCreateBinary();
    if (!combo_timer_sem) {
        ESP_LOGE(TAG, "Failed to create timer semaphore");
        return;
    }

    // Create and start 1ms periodic timer
    const esp_timer_create_args_t timer_args = {
        .callback = &combo_timer_callback,
        .name = "combo_pacer",
        .dispatch_method = ESP_TIMER_TASK
    };
    esp_timer_handle_t combo_timer;
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &combo_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(combo_timer, 1000)); // 1ms = 1000us

    ESP_LOGI(TAG, "COMBO initialized, registering for network startup notification");

    // Wait for mesh network to be ready via event notification (not polling)
    // Ready when: (1) connected to existing mesh, OR (2) becomes root and initializes
    // Network layer notifies us immediately when ready
    ESP_ERROR_CHECK(network_register_startup_notification(xTaskGetCurrentTaskHandle()));
    uint32_t notify_value = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (notify_value > 0) {
        ESP_LOGI(TAG, "Network ready - starting audio transmission");
    }

    uint32_t bytes_sent = 0;
    static uint16_t combo_seq = 0;

    while (1) {
        // Wait for 1ms timer tick (but only send every 10ms)
        xSemaphoreTake(combo_timer_sem, portMAX_DELAY);
        ms_tick++;

        // Handle button events (every 5ms for responsiveness)
        if ((ms_tick % 5) == 0) {
            button_event_t btn_event = buttons_poll();
            if (btn_event == BUTTON_EVENT_SHORT_PRESS) {
                current_view = (current_view == DISPLAY_VIEW_NETWORK) ?
                              DISPLAY_VIEW_AUDIO : DISPLAY_VIEW_NETWORK;
                ESP_LOGI(TAG, "View changed to %s",
                        current_view == DISPLAY_VIEW_NETWORK ? "Network" : "Audio");
            } else if (btn_event == BUTTON_EVENT_LONG_PRESS) {
                input_mode_t old_mode = status.input_mode;
                status.input_mode = (status.input_mode + 1) % 3;

                // Manage ADC continuous mode based on input mode
                if (old_mode == INPUT_MODE_AUX && status.input_mode != INPUT_MODE_AUX) {
                    // Leaving AUX mode - stop continuous ADC
                    adc_audio_stop();
                    ESP_LOGI(TAG, "Input mode changed to %d (ADC stopped)", status.input_mode);
                } else if (old_mode != INPUT_MODE_AUX && status.input_mode == INPUT_MODE_AUX) {
                    // Entering AUX mode - start continuous ADC
                    adc_audio_start();
                    ESP_LOGI(TAG, "Input mode changed to %d (ADC started)", status.input_mode);
                } else {
                    ESP_LOGI(TAG, "Input mode changed to %d", status.input_mode);
                }
            }
        }

        // Generate/capture audio based on mode (every 10ms to match frame size)
        if ((ms_tick % AUDIO_FRAME_MS) != 0) {
            // Update controls every 1ms for responsiveness, but don't generate audio
            if (status.input_mode == INPUT_MODE_TONE) {
                update_tone_oscillate();
            }
            continue; // Skip non-frame ticks for audio generation
        }

        status.audio_active = false;
        switch (status.input_mode) {
        case INPUT_MODE_TONE:
            tone_gen_fill_buffer(mono_frame, AUDIO_FRAME_SAMPLES);
            // Convert mono to stereo for local I2S output
            for (size_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
                stereo_frame[i * 2] = stereo_frame[i * 2 + 1] = mono_frame[i];
            }
            // Pack 16-bit mono → 24-bit mono for network transmission
            pcm16_mono_to_pcm24_mono_pack(mono_frame, AUDIO_FRAME_SAMPLES, packet_buffer);
            status.audio_active = true;
            break;
        case INPUT_MODE_USB:
            if (usb_audio_is_active()) {
                size_t frames_read;
                usb_audio_read_frames(stereo_frame, AUDIO_FRAME_SAMPLES, &frames_read);
                if (frames_read > 0) {
                    // Pack 16-bit stereo → 24-bit mono for network (downmix L+R)
                    pcm16_stereo_to_pcm24_mono_pack(stereo_frame, frames_read, packet_buffer);
                    // Pad remaining with silence
                    for (size_t i = frames_read; i < AUDIO_FRAME_SAMPLES; i++) {
                        s24le_pack(0, &packet_buffer[i * 3]);
                    }
                    status.audio_active = true;
                } else {
                    memset(packet_buffer, 0, AUDIO_FRAME_BYTES);
                }
            }
            break;
        case INPUT_MODE_AUX:
            {
                size_t samples_read = 0;
                esp_err_t ret = adc_audio_read_stereo(stereo_frame, AUDIO_FRAME_SAMPLES, &samples_read);

                if (ret == ESP_OK && samples_read > 0) {
                    // Fill remaining samples with last value if needed
                    if (samples_read < AUDIO_FRAME_SAMPLES) {
                        int16_t last_left = stereo_frame[(samples_read - 1) * 2];
                        int16_t last_right = stereo_frame[(samples_read - 1) * 2 + 1];
                        for (size_t i = samples_read; i < AUDIO_FRAME_SAMPLES; i++) {
                            stereo_frame[i * 2] = last_left;
                            stereo_frame[i * 2 + 1] = last_right;
                        }
                    }

                    // Detect actual audio by measuring AC variance
                    int64_t sum_left = 0, sum_right = 0;
                    for (size_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
                        sum_left += stereo_frame[i * 2];
                        sum_right += stereo_frame[i * 2 + 1];
                    }
                    int32_t mean_left = (int32_t)(sum_left / AUDIO_FRAME_SAMPLES);
                    int32_t mean_right = (int32_t)(sum_right / AUDIO_FRAME_SAMPLES);

                    int64_t var_left = 0, var_right = 0;
                    for (size_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
                        int32_t diff_l = stereo_frame[i * 2] - mean_left;
                        int32_t diff_r = stereo_frame[i * 2 + 1] - mean_right;
                        var_left += (int64_t)diff_l * diff_l;
                        var_right += (int64_t)diff_r * diff_r;
                    }
                    int32_t std_left = (int32_t)sqrt(var_left / AUDIO_FRAME_SAMPLES);
                    int32_t std_right = (int32_t)sqrt(var_right / AUDIO_FRAME_SAMPLES);
                    int32_t std_avg = (std_left + std_right) / 2;

                    const int32_t SIGNAL_THRESHOLD = 10;

                    if ((ms_tick & 0xFF) == 0) {
                        ESP_LOGI(TAG, "AUX: STD=%ld, DC_L=%ld, DC_R=%ld", std_avg, mean_left, mean_right);
                    }

                    if (std_avg > SIGNAL_THRESHOLD) {
                        // Pack 16-bit stereo → 24-bit mono for network (downmix L+R)
                        pcm16_stereo_to_pcm24_mono_pack(stereo_frame, samples_read, packet_buffer);
                        status.audio_active = true;
                    } else {
                        memset(packet_buffer, 0, AUDIO_FRAME_BYTES);
                        status.audio_active = false;
                    }
                } else {
                    memset(packet_buffer, 0, AUDIO_FRAME_BYTES);
                    status.audio_active = false;
                    if (ret != ESP_OK) {
                        ESP_LOGW(TAG, "AUX: ADC read error: %s", esp_err_to_name(ret));
                    }
                }
            }
            break;
        default:
            break;
        }

        // Apply volume scaling for AUX and USB modes
        if (status.input_mode == INPUT_MODE_AUX || status.input_mode == INPUT_MODE_USB) {
            for (size_t i = 0; i < AUDIO_FRAME_SAMPLES * 2; i++) {
                stereo_frame[i] = (int16_t)(stereo_frame[i] * status.output_volume);
            }
        }

        // Output audio directly to I2S (UDA1334)
        if (status.audio_active) {
            i2s_audio_write_samples(stereo_frame, AUDIO_FRAME_SAMPLES * 2);
            if ((ms_tick & 0x7F) == 0) {
                ESP_LOGI(TAG, "Output frame to I2S");
            }
        } else {
            // Output silence
            memset(stereo_frame, 0, sizeof(stereo_frame));
            i2s_audio_write_samples(stereo_frame, AUDIO_FRAME_SAMPLES * 2);
        }

        // Transmit audio to mesh network when ready
        // Payload format (v0.1): PCM S24LE packed, mono, 48 kHz, 5ms frames (720 bytes)
        // Only attempt send if both audio is active AND mesh is fully ready
        if (status.audio_active && network_is_stream_ready()) {
            // packet_buffer already contains 24-bit packed mono data from conversion above

            // Build frame header
            net_frame_header_t hdr;
            hdr.magic = NET_FRAME_MAGIC;
            hdr.version = NET_FRAME_VERSION;
            hdr.type = NET_PKT_TYPE_AUDIO_RAW;
            hdr.stream_id = 1;  // Stream ID (will be set by network layer)
            hdr.seq = htons(combo_seq++);
            hdr.timestamp = htonl((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
            hdr.payload_len = htons((uint16_t)AUDIO_FRAME_BYTES);
            hdr.ttl = 6;  // Max 6 hops
            hdr.reserved = 0;

            // Copy header + payload to framed buffer
            memcpy(framed_buffer, &hdr, NET_FRAME_HEADER_SIZE);
            memcpy(framed_buffer + NET_FRAME_HEADER_SIZE, packet_buffer, AUDIO_FRAME_BYTES);

            esp_err_t send_ret = network_send_audio(framed_buffer, NET_FRAME_HEADER_SIZE + AUDIO_FRAME_BYTES);
            if (send_ret == ESP_OK) {
                bytes_sent += (NET_FRAME_HEADER_SIZE + AUDIO_FRAME_BYTES);
                if ((combo_seq & 0x7F) == 0) {
                    ESP_LOGI(TAG, "Sent packet seq=%u (%d bytes)", ntohs(hdr.seq), NET_FRAME_HEADER_SIZE + AUDIO_FRAME_BYTES);
                }
            } else if (send_ret != ESP_ERR_MESH_DISCONNECTED) {
                // Only warn on errors other than disconnected (expected for standalone root)
                if ((combo_seq & 0x7F) == 0) {
                    ESP_LOGW(TAG, "Failed to send: %s", esp_err_to_name(send_ret));
                }
            }
        }

        // Update network stats every second
        uint32_t now = xTaskGetTickCount();
        static uint32_t last_stats_update = 0;
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

        // Update display at 10 Hz (every 100ms)
        if ((ms_tick % 100) == 0) {
            display_render_combo(current_view, &status);
        }

        // Reset watchdog
        esp_task_wdt_reset();
    }
}
