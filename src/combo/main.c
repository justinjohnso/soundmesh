#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_log.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <esp_mesh.h>
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
#include "audio/ring_buffer.h"
#include "network/mesh_net.h"

#ifdef CONFIG_USE_ES8388
#include "audio/es8388_audio.h"
#else
#include "audio/adc_audio.h"
#include "audio/i2s_audio.h"
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#endif

static const char *TAG = "combo_main";

_Static_assert(AUDIO_BITS_PER_SAMPLE == 24 && AUDIO_CHANNELS == 1, "v0.1 requires 24-bit mono");

static inline void s24le_pack(int32_t s24, uint8_t* out) {
    out[0] = (uint8_t)(s24 & 0xFF);
    out[1] = (uint8_t)((s24 >> 8) & 0xFF);
    out[2] = (uint8_t)((s24 >> 16) & 0xFF);
}

static void pcm16_mono_to_pcm24_mono_pack(const int16_t* in, size_t frames, uint8_t* out) {
    for (size_t i = 0; i < frames; i++) {
        int32_t s24 = ((int32_t)in[i]) << 8;
        s24le_pack(s24, &out[i * 3]);
    }
}

static void pcm16_stereo_to_pcm24_mono_pack(const int16_t* in_lr, size_t frames, uint8_t* out) {
    for (size_t i = 0; i < frames; i++) {
        int32_t m = ((int32_t)in_lr[i*2] + (int32_t)in_lr[i*2 + 1]) >> 1;
        int32_t s24 = m << 8;
        s24le_pack(s24, &out[i * 3]);
    }
}

static void pcm32_stereo_to_pcm24_mono_pack(const int32_t* in_lr, size_t frames, uint8_t* out) {
    for (size_t i = 0; i < frames; i++) {
        // ES8388 32-bit I2S outputs 24-bit audio LEFT-JUSTIFIED (MSB-aligned)
        // Data is in upper 24 bits, lower 8 bits are zero padding
        // Shift right by 8 to get actual 24-bit signed value
        int32_t left = in_lr[i*2] >> 8;
        int32_t right = in_lr[i*2 + 1] >> 8;
        int32_t mono = (left + right) >> 1;
        s24le_pack(mono & 0x00FFFFFF, &out[i * 3]);
    }
}



static combo_status_t status = {
    .input_mode = INPUT_MODE_AUX,
    .audio_active = false,
    .connected_nodes = 0,
    .latency_ms = 10,
    .bandwidth_kbps = 0,
    .rssi = 0,
    .tone_freq_hz = 440,
    .output_volume = 1.0f
};

static display_view_t current_view = DISPLAY_VIEW_AUDIO;
static ring_buffer_t *audio_buffer = NULL;

static int16_t mono_frame[AUDIO_FRAME_SAMPLES];
static int16_t stereo_frame_16[AUDIO_FRAME_SAMPLES * 2];  // For USB audio (16-bit)
static int32_t stereo_frame_32[AUDIO_FRAME_SAMPLES * 2];  // For ES8388 (24-bit in 32-bit containers)
static uint8_t packet_buffer[AUDIO_FRAME_BYTES];
static uint8_t framed_buffer[NET_FRAME_HEADER_SIZE + AUDIO_FRAME_BYTES];

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
    ESP_LOGI(TAG, "MeshNet Audio COMBO starting...");

    // Raise main task priority above default (1) to reduce starvation, but below network tasks
    // WiFi runs at 18-23, mesh RX at 5 - we run at 10 (audio can hiccup, network should not)
    vTaskPrioritySet(NULL, 10);

#ifdef CONFIG_USE_ES8388
    ESP_LOGI(TAG, "Audio input: ES8388 codec (LIN2/RIN2)");
    ESP_LOGI(TAG, "Audio output: ES8388 headphone (monitor)");
#else
    ESP_LOGI(TAG, "Audio input: ADC (legacy)");
    ESP_LOGI(TAG, "Audio output: UDA1334 DAC");
#endif

    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(buttons_init());

    ESP_ERROR_CHECK(network_init_mesh());
    ESP_ERROR_CHECK(network_start_latency_measurement());

    ESP_ERROR_CHECK(tone_gen_init(status.tone_freq_hz));
    ESP_ERROR_CHECK(usb_audio_init());

#ifdef CONFIG_USE_ES8388
    // Initialize ES8388 with DAC enabled for headphone monitor output
    // Note: Volume is set in es8388_codec_init() BEFORE I2S starts (I2C fails after due to MCLK EMI)
    ESP_ERROR_CHECK(es8388_audio_init(true));
#else
    ESP_ERROR_CHECK(adc_audio_init());
    ESP_ERROR_CHECK(i2s_audio_init());
#endif

    audio_buffer = ring_buffer_create(RING_BUFFER_SIZE);
    if (!audio_buffer) {
        ESP_LOGE(TAG, "Failed to create ring buffer");
        return;
    }

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

    ESP_LOGI(TAG, "COMBO initialized, registering for network startup notification");

    ESP_ERROR_CHECK(network_register_startup_notification(xTaskGetCurrentTaskHandle()));
    uint32_t notify_value = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (notify_value > 0) {
        ESP_LOGI(TAG, "Network ready - starting audio transmission");
    }

    uint32_t bytes_sent = 0;
    static uint16_t combo_seq = 0;
    
    // Real-time scheduling using esp_timer_get_time() instead of semaphore ticks
    int64_t last_audio_ms = esp_timer_get_time() / 1000;
    int64_t last_button_ms = last_audio_ms;
    int64_t last_display_ms = last_audio_ms;
    int64_t last_stats_ms = last_audio_ms;
    static uint32_t frame_count = 0;

    while (1) {
        // Yield to other tasks briefly
        vTaskDelay(pdMS_TO_TICKS(1));
        
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
                input_mode_t old_mode = status.input_mode;
                status.input_mode = (status.input_mode + 1) % 3;
                ESP_LOGI(TAG, "Input mode changed to %d", status.input_mode);

#ifndef CONFIG_USE_ES8388
                if (old_mode == INPUT_MODE_AUX && status.input_mode != INPUT_MODE_AUX) {
                    adc_audio_stop();
                } else if (old_mode != INPUT_MODE_AUX && status.input_mode == INPUT_MODE_AUX) {
                    adc_audio_start();
                }
#endif
            }
        }
        
        // Tone oscillation update (runs frequently for smooth modulation)
        if (status.input_mode == INPUT_MODE_TONE) {
            update_tone_oscillate(now_ms);
        }

        // Audio frame processing every AUDIO_FRAME_MS (10ms)
        if (now_ms - last_audio_ms < AUDIO_FRAME_MS) {
            esp_task_wdt_reset();
            continue;
        }
        last_audio_ms += AUDIO_FRAME_MS;  // Advance by frame period to maintain timing
        frame_count++;

        status.audio_active = false;
        
        switch (status.input_mode) {
        case INPUT_MODE_TONE:
            tone_gen_fill_buffer(mono_frame, AUDIO_FRAME_SAMPLES);
            for (size_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
                // Convert 16-bit tone to 32-bit left-justified for ES8388 DAC
                // Shift by 16 to put 16-bit audio in upper 16 bits of 32-bit word
                stereo_frame_32[i * 2] = stereo_frame_32[i * 2 + 1] = ((int32_t)mono_frame[i]) << 16;
            }
            pcm16_mono_to_pcm24_mono_pack(mono_frame, AUDIO_FRAME_SAMPLES, packet_buffer);
            status.audio_active = true;
            break;

        case INPUT_MODE_USB:
            if (usb_audio_is_active()) {
                size_t frames_read;
                usb_audio_read_frames(stereo_frame_16, AUDIO_FRAME_SAMPLES, &frames_read);
                if (frames_read > 0) {
                    pcm16_stereo_to_pcm24_mono_pack(stereo_frame_16, frames_read, packet_buffer);
                    // Convert 16-bit USB to 32-bit left-justified for ES8388 DAC
                    // Shift by 16 to put 16-bit audio in upper 16 bits of 32-bit word
                    for (size_t i = 0; i < frames_read; i++) {
                        stereo_frame_32[i * 2] = ((int32_t)stereo_frame_16[i * 2]) << 16;
                        stereo_frame_32[i * 2 + 1] = ((int32_t)stereo_frame_16[i * 2 + 1]) << 16;
                    }
                    for (size_t i = frames_read; i < AUDIO_FRAME_SAMPLES; i++) {
                        s24le_pack(0, &packet_buffer[i * 3]);
                        stereo_frame_32[i * 2] = stereo_frame_32[i * 2 + 1] = 0;
                    }
                    status.audio_active = true;
                } else {
                    memset(packet_buffer, 0, AUDIO_FRAME_BYTES);
                    memset(stereo_frame_32, 0, sizeof(stereo_frame_32));
                }
            }
            break;

        case INPUT_MODE_AUX:
            {
                size_t samples_read = 0;
#ifdef CONFIG_USE_ES8388
                esp_err_t ret = es8388_audio_read_stereo(stereo_frame_32, AUDIO_FRAME_SAMPLES, &samples_read);
#else
                esp_err_t ret = adc_audio_read_stereo(stereo_frame_16, AUDIO_FRAME_SAMPLES, &samples_read);
#endif

                if (ret == ESP_OK && samples_read > 0) {
#ifdef CONFIG_USE_ES8388
                    // ES8388 returns 24-bit samples in 32-bit containers
                    if (samples_read < AUDIO_FRAME_SAMPLES) {
                        int32_t last_left = stereo_frame_32[(samples_read - 1) * 2];
                        int32_t last_right = stereo_frame_32[(samples_read - 1) * 2 + 1];
                        for (size_t i = samples_read; i < AUDIO_FRAME_SAMPLES; i++) {
                            stereo_frame_32[i * 2] = last_left;
                            stereo_frame_32[i * 2 + 1] = last_right;
                        }
                    }

                    // ES8388 32-bit I2S: 24-bit audio is LEFT-JUSTIFIED (MSB-aligned)
                    // Shift right by 8 to get actual 24-bit signed values for statistics
                    int64_t sum_left = 0, sum_right = 0;
                    for (size_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
                        sum_left += stereo_frame_32[i * 2] >> 8;
                        sum_right += stereo_frame_32[i * 2 + 1] >> 8;
                    }
                    int32_t mean_left = (int32_t)(sum_left / AUDIO_FRAME_SAMPLES);
                    int32_t mean_right = (int32_t)(sum_right / AUDIO_FRAME_SAMPLES);

                    int64_t var_left = 0, var_right = 0;
                    for (size_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
                        int32_t val_l = stereo_frame_32[i * 2] >> 8;
                        int32_t val_r = stereo_frame_32[i * 2 + 1] >> 8;
                        int32_t diff_l = val_l - mean_left;
                        int32_t diff_r = val_r - mean_right;
                        var_left += (int64_t)diff_l * diff_l;
                        var_right += (int64_t)diff_r * diff_r;
                    }
                    int32_t std_left = (int32_t)sqrt((double)(var_left / AUDIO_FRAME_SAMPLES));
                    int32_t std_right = (int32_t)sqrt((double)(var_right / AUDIO_FRAME_SAMPLES));
                    int32_t std_avg = (std_left + std_right) / 2;

                    // Threshold for 24-bit audio (~-60dB = ~8000, ~-40dB = ~80000)
                    const int32_t SIGNAL_THRESHOLD = 5000;

                    if ((frame_count & 0x3F) == 0) {
                        ESP_LOGI(TAG, "AUX: STD=%ld, DC_L=%ld, DC_R=%ld", std_avg, mean_left, mean_right);
                        // Debug: show raw 32-bit samples and shifted 24-bit values
                        int32_t s0l = stereo_frame_32[0] >> 8;
                        int32_t s0r = stereo_frame_32[1] >> 8;
                        ESP_LOGI(TAG, "RAW32[0]: 0x%08lX/0x%08lX -> 24bit: %ld/%ld",
                                 (uint32_t)stereo_frame_32[0], (uint32_t)stereo_frame_32[1], s0l, s0r);
                    }

                    if (std_avg > SIGNAL_THRESHOLD) {
                        pcm32_stereo_to_pcm24_mono_pack(stereo_frame_32, samples_read, packet_buffer);
                        status.audio_active = true;
                    } else {
                        memset(packet_buffer, 0, AUDIO_FRAME_BYTES);
                        status.audio_active = false;
                    }
#else
                    // Legacy ADC path (16-bit)
                    if (samples_read < AUDIO_FRAME_SAMPLES) {
                        int16_t last_left = stereo_frame_16[(samples_read - 1) * 2];
                        int16_t last_right = stereo_frame_16[(samples_read - 1) * 2 + 1];
                        for (size_t i = samples_read; i < AUDIO_FRAME_SAMPLES; i++) {
                            stereo_frame_16[i * 2] = last_left;
                            stereo_frame_16[i * 2 + 1] = last_right;
                        }
                    }

                    int64_t sum_left = 0, sum_right = 0;
                    for (size_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
                        sum_left += stereo_frame_16[i * 2];
                        sum_right += stereo_frame_16[i * 2 + 1];
                    }
                    int32_t mean_left = (int32_t)(sum_left / AUDIO_FRAME_SAMPLES);
                    int32_t mean_right = (int32_t)(sum_right / AUDIO_FRAME_SAMPLES);

                    int64_t var_left = 0, var_right = 0;
                    for (size_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
                        int32_t diff_l = stereo_frame_16[i * 2] - mean_left;
                        int32_t diff_r = stereo_frame_16[i * 2 + 1] - mean_right;
                        var_left += (int64_t)diff_l * diff_l;
                        var_right += (int64_t)diff_r * diff_r;
                    }
                    int32_t std_left = (int32_t)sqrt(var_left / AUDIO_FRAME_SAMPLES);
                    int32_t std_right = (int32_t)sqrt(var_right / AUDIO_FRAME_SAMPLES);
                    int32_t std_avg = (std_left + std_right) / 2;

                    const int32_t SIGNAL_THRESHOLD = 10;

                    if ((frame_count & 0x3F) == 0) {
                        ESP_LOGI(TAG, "AUX: STD=%ld, DC_L=%ld, DC_R=%ld", std_avg, mean_left, mean_right);
                    }

                    if (std_avg > SIGNAL_THRESHOLD) {
                        pcm16_stereo_to_pcm24_mono_pack(stereo_frame_16, samples_read, packet_buffer);
                        status.audio_active = true;
                    } else {
                        memset(packet_buffer, 0, AUDIO_FRAME_BYTES);
                        status.audio_active = false;
                    }
#endif
                } else {
                    memset(packet_buffer, 0, AUDIO_FRAME_BYTES);
                    status.audio_active = false;
                    if (ret != ESP_OK) {
                        ESP_LOGW(TAG, "AUX: read error: %s", esp_err_to_name(ret));
                    }
                }
            }
            break;

        default:
            break;
        }

        // Apply volume scaling to 32-bit stereo buffer
        if (status.input_mode == INPUT_MODE_AUX || status.input_mode == INPUT_MODE_USB) {
            for (size_t i = 0; i < AUDIO_FRAME_SAMPLES * 2; i++) {
                stereo_frame_32[i] = (int32_t)(stereo_frame_32[i] * status.output_volume);
            }
        }

        // Output to local headphone monitor (ES8388 DAC or UDA1334)
        if (status.audio_active) {
#ifdef CONFIG_USE_ES8388
            es8388_audio_write_stereo(stereo_frame_32, AUDIO_FRAME_SAMPLES);
#else
            i2s_audio_write_samples(stereo_frame_16, AUDIO_FRAME_SAMPLES * 2);
#endif
            if ((frame_count & 0x3F) == 0) {
                ESP_LOGI(TAG, "Output frame to headphones");
            }
        } else {
            memset(stereo_frame_32, 0, sizeof(stereo_frame_32));
#ifdef CONFIG_USE_ES8388
            es8388_audio_write_stereo(stereo_frame_32, AUDIO_FRAME_SAMPLES);
#else
            memset(stereo_frame_16, 0, sizeof(stereo_frame_16));
            i2s_audio_write_samples(stereo_frame_16, AUDIO_FRAME_SAMPLES * 2);
#endif
        }

        // Transmit to mesh network
        if (status.audio_active && network_is_stream_ready()) {
            net_frame_header_t hdr;
            hdr.magic = NET_FRAME_MAGIC;
            hdr.version = NET_FRAME_VERSION;
            hdr.type = NET_PKT_TYPE_AUDIO_RAW;
            hdr.stream_id = 1;
            hdr.seq = htons(combo_seq++);
            hdr.timestamp = htonl((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
            hdr.payload_len = htons((uint16_t)AUDIO_FRAME_BYTES);
            hdr.ttl = 6;
            hdr.reserved = 0;

            memcpy(framed_buffer, &hdr, NET_FRAME_HEADER_SIZE);
            memcpy(framed_buffer + NET_FRAME_HEADER_SIZE, packet_buffer, AUDIO_FRAME_BYTES);

            esp_err_t send_ret = network_send_audio(framed_buffer, NET_FRAME_HEADER_SIZE + AUDIO_FRAME_BYTES);
            if (send_ret == ESP_OK) {
                bytes_sent += (NET_FRAME_HEADER_SIZE + AUDIO_FRAME_BYTES);
                if ((combo_seq & 0x7F) == 0) {
                    ESP_LOGI(TAG, "Sent packet seq=%u (%d bytes)", ntohs(hdr.seq), NET_FRAME_HEADER_SIZE + AUDIO_FRAME_BYTES);
                }
            } else if (send_ret != ESP_ERR_MESH_DISCONNECTED) {
                if ((combo_seq & 0x7F) == 0) {
                    ESP_LOGW(TAG, "Failed to send: %s", esp_err_to_name(send_ret));
                }
            }
        }

        // Stats update every 1000ms
        if (now_ms - last_stats_ms >= 1000) {
            int64_t elapsed_ms = now_ms - last_stats_ms;
            if (elapsed_ms > 0 && bytes_sent > 0) {
                status.bandwidth_kbps = (bytes_sent * 8) / (uint32_t)elapsed_ms;
            }
            status.connected_nodes = network_get_connected_nodes();
            status.rssi = network_get_rssi();
            status.latency_ms = network_get_latency_ms();
            last_stats_ms = now_ms;
            bytes_sent = 0;
        }

        // Display update every 100ms
        if (now_ms - last_display_ms >= 100) {
            last_display_ms = now_ms;
            display_render_combo(current_view, &status);
        }

        esp_task_wdt_reset();
    }
}
