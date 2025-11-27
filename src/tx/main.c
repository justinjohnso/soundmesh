#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_log.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
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
#include "network/mesh_net.h"
#include "audio/tone_gen.h"
#include "audio/usb_audio.h"
#include "audio/ring_buffer.h"

#ifdef CONFIG_USE_ES8388
#include "audio/es8388_audio.h"
#else
#include "audio/adc_audio.h"
#endif

static const char *TAG = "tx_main";

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

static SemaphoreHandle_t tx_timer_sem = NULL;
static uint32_t ms_tick = 0;

static void tx_timer_callback(void* arg) {
    xSemaphoreGive(tx_timer_sem);
}

static tx_status_t status = {
    .input_mode = INPUT_MODE_TONE,
    .audio_active = false,
    .connected_nodes = 0,
    .latency_ms = 10,
    .bandwidth_kbps = 0,
    .rssi = 0,
    .tone_freq_hz = 110
};

static display_view_t current_view = DISPLAY_VIEW_NETWORK;
static ring_buffer_t *audio_buffer = NULL;

#ifndef CONFIG_USE_ES8388
static adc_oneshot_unit_handle_t adc1_handle;
static adc_cali_handle_t adc1_cali_handle = NULL;
#endif

static int16_t mono_frame[AUDIO_FRAME_SAMPLES];
static int16_t stereo_frame_16[AUDIO_FRAME_SAMPLES * 2];  // For USB audio (16-bit)
static int32_t stereo_frame_32[AUDIO_FRAME_SAMPLES * 2];  // For ES8388 (24-bit in 32-bit containers)
static uint8_t packet_buffer[AUDIO_FRAME_BYTES];
static uint8_t framed_buffer[NET_FRAME_HEADER_SIZE + AUDIO_FRAME_BYTES];
static uint16_t tx_seq = 0;

#ifndef CONFIG_USE_ES8388
void update_tone_from_adc(void) {
    static uint32_t last_log = 0;
    static uint32_t read_count = 0;
    
    if (adc1_handle == NULL || adc1_cali_handle == NULL) {
        return;
    }

    int adc_raw;
    esp_err_t ret = adc_oneshot_read(adc1_handle, ADC_CHANNEL_3, &adc_raw);
    if (ret != ESP_OK) {
        if ((read_count % 1000) == 0) {
            ESP_LOGW(TAG, "ADC read failed: %s", esp_err_to_name(ret));
        }
        read_count++;
        return;
    }

    int voltage_mv;
    if (adc_cali_raw_to_voltage(adc1_cali_handle, adc_raw, &voltage_mv) != ESP_OK) {
        return;
    }

    uint32_t new_freq = 200 + ((voltage_mv * 1800) / 3300);
    
    uint32_t now = xTaskGetTickCount();
    if ((now - last_log) > pdMS_TO_TICKS(2000)) {
        ESP_LOGI(TAG, "Knob: raw=%d, mv=%d, freq=%lu Hz", adc_raw, voltage_mv, new_freq);
        last_log = now;
    }
    
    if (abs((int)new_freq - (int)status.tone_freq_hz) > 5) {
        status.tone_freq_hz = new_freq;
        tone_gen_set_frequency(status.tone_freq_hz);
        ESP_LOGI(TAG, "Tone frequency updated to %lu Hz", status.tone_freq_hz);
    }
    
    read_count++;
}
#else
void update_tone_oscillate(void) {
    static uint32_t last_log = 0;
    
    uint32_t phase = (ms_tick / 20) % 200;
    float ratio = (float)phase / 200.0f;
    float sine_val = sinf(ratio * 2.0f * M_PI);
    uint32_t center = 500;
    uint32_t range = 200;
    uint32_t new_freq = center + (uint32_t)(sine_val * range);
    
    if (abs((int)new_freq - (int)status.tone_freq_hz) > 5) {
        status.tone_freq_hz = new_freq;
        tone_gen_set_frequency(status.tone_freq_hz);
    }
    
    uint32_t now = xTaskGetTickCount();
    if ((now - last_log) > pdMS_TO_TICKS(2000)) {
        ESP_LOGI(TAG, "Tone oscillating: freq=%lu Hz", status.tone_freq_hz);
        last_log = now;
    }
}
#endif

void app_main(void) {
    ESP_LOGI(TAG, "MeshNet Audio TX starting...");
    
    // Raise main task priority above default (1) to reduce starvation, but below network tasks
    vTaskPrioritySet(NULL, 10);
    
#ifdef CONFIG_USE_ES8388
    ESP_LOGI(TAG, "Audio input: ES8388 codec (LIN2/RIN2)");
#else
    ESP_LOGI(TAG, "Audio input: ADC (legacy)");
#endif

    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(buttons_init());
    
    ESP_ERROR_CHECK(network_init_mesh());
    ESP_ERROR_CHECK(network_start_latency_measurement());

#ifndef CONFIG_USE_ES8388
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_3, &config));

    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .chan = ADC_CHANNEL_3,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cali_config, &adc1_cali_handle));
#endif

    ESP_ERROR_CHECK(tone_gen_init(status.tone_freq_hz));
    ESP_ERROR_CHECK(usb_audio_init());

#ifdef CONFIG_USE_ES8388
    // TX mode: ES8388 ADC only (no DAC output needed)
    ESP_ERROR_CHECK(es8388_audio_init(false));
#else
    ESP_ERROR_CHECK(adc_audio_init());
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

    tx_timer_sem = xSemaphoreCreateBinary();
    if (!tx_timer_sem) {
        ESP_LOGE(TAG, "Failed to create timer semaphore");
        return;
    }
    
    const esp_timer_create_args_t timer_args = {
        .callback = &tx_timer_callback,
        .name = "tx_pacer",
        .dispatch_method = ESP_TIMER_TASK
    };
    esp_timer_handle_t tx_timer;
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &tx_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tx_timer, 1000));
    
    ESP_LOGI(TAG, "TX initialized, registering for network startup notification");
    
    ESP_ERROR_CHECK(network_register_startup_notification(xTaskGetCurrentTaskHandle()));
    uint32_t notify_value = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (notify_value > 0) {
        ESP_LOGI(TAG, "Network ready - starting audio transmission");
    }

    uint32_t bytes_sent = 0;
    uint32_t last_stats_update = xTaskGetTickCount();
    
    while (1) {
        xSemaphoreTake(tx_timer_sem, portMAX_DELAY);
        ms_tick++;
        
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
        
        if ((ms_tick % AUDIO_FRAME_MS) != 0) {
            if (status.input_mode == INPUT_MODE_TONE) {
#ifdef CONFIG_USE_ES8388
                update_tone_oscillate();
#else
                update_tone_from_adc();
#endif
            }
            continue;
        }
        
        status.audio_active = false;
        switch (status.input_mode) {
        case INPUT_MODE_TONE:
            tone_gen_fill_buffer(mono_frame, AUDIO_FRAME_SAMPLES);
            pcm16_mono_to_pcm24_mono_pack(mono_frame, AUDIO_FRAME_SAMPLES, packet_buffer);
            status.audio_active = true;
            break;

        case INPUT_MODE_USB:
            if (usb_audio_is_active()) {
                size_t frames_read;
                usb_audio_read_frames(stereo_frame_16, AUDIO_FRAME_SAMPLES, &frames_read);
                if (frames_read > 0) {
                    pcm16_stereo_to_pcm24_mono_pack(stereo_frame_16, frames_read, packet_buffer);
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
                    
                    // Threshold for 24-bit audio (~-60dB = ~8000)
                    const int32_t SIGNAL_THRESHOLD = 5000;
                    
                    if (std_avg > SIGNAL_THRESHOLD) {
                        pcm32_stereo_to_pcm24_mono_pack(stereo_frame_32, samples_read, packet_buffer);
                        status.audio_active = true;
                        if ((tx_seq & 0xFF) == 0) {
                            ESP_LOGI(TAG, "AUX: STD=%ld, DC_L=%ld, DC_R=%ld", std_avg, mean_left, mean_right);
                        }
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
                    
                    const int32_t SIGNAL_THRESHOLD = 500;
                    
                    if (std_avg > SIGNAL_THRESHOLD) {
                        pcm16_stereo_to_pcm24_mono_pack(stereo_frame_16, samples_read, packet_buffer);
                        status.audio_active = true;
                        if ((tx_seq & 0xFF) == 0) {
                            ESP_LOGI(TAG, "AUX: STD=%ld, DC_L=%ld, DC_R=%ld", std_avg, mean_left, mean_right);
                        }
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
        
        if (status.audio_active && network_is_stream_ready()) {
            net_frame_header_t hdr;
            hdr.magic = NET_FRAME_MAGIC;
            hdr.version = NET_FRAME_VERSION;
            hdr.type = NET_PKT_TYPE_AUDIO_RAW;
            hdr.stream_id = 1;
            hdr.seq = htons(tx_seq++);
            hdr.timestamp = htonl((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
            hdr.payload_len = htons((uint16_t)AUDIO_FRAME_BYTES);
            hdr.ttl = 6;
            hdr.reserved = 0;

            memcpy(framed_buffer, &hdr, NET_FRAME_HEADER_SIZE);
            memcpy(framed_buffer + NET_FRAME_HEADER_SIZE, packet_buffer, AUDIO_FRAME_BYTES);

            esp_err_t send_ret = network_send_audio(framed_buffer, NET_FRAME_HEADER_SIZE + AUDIO_FRAME_BYTES);
            if (send_ret == ESP_OK) {
                bytes_sent += (NET_FRAME_HEADER_SIZE + AUDIO_FRAME_BYTES);
                if ((tx_seq & 0x7F) == 0) {
                    ESP_LOGI(TAG, "Sent packet seq=%u (%d bytes)", ntohs(hdr.seq), NET_FRAME_HEADER_SIZE + AUDIO_FRAME_BYTES);
                }
            } else {
                if ((tx_seq & 0x7F) == 0) {
                    ESP_LOGW(TAG, "Failed to send: %s", esp_err_to_name(send_ret));
                }
            }
        }

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
            bytes_sent = 0;
        }

        if ((ms_tick % 100) == 0) {
            display_render_tx(current_view, &status);
        }

        esp_task_wdt_reset();
    }
}
