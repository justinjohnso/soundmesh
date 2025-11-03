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
#include "audio/adc_audio.h"
// #include "audio/opus_codec.h"  // Removed for now
#include "audio/ring_buffer.h"

static const char *TAG = "tx_main";

// Timer for 1ms pacing
static SemaphoreHandle_t tx_timer_sem = NULL;
static uint32_t ms_tick = 0;

static void tx_timer_callback(void* arg) {
    xSemaphoreGive(tx_timer_sem);
}

static tx_status_t status = {
.input_mode = INPUT_MODE_TONE,  // Start in TONE mode so ADC continuous doesn't block knob
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
    static uint32_t last_log = 0;
    static uint32_t read_count = 0;
    
    if (adc1_handle == NULL || adc1_cali_handle == NULL) {
        return; // ADC not initialized
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

    // Map voltage (0-3300mV) to frequency (200-2000Hz)
    uint32_t new_freq = 200 + ((voltage_mv * 1800) / 3300);
    
    // Log periodically
    uint32_t now = xTaskGetTickCount();
    if ((now - last_log) > pdMS_TO_TICKS(2000)) {
        ESP_LOGI(TAG, "Knob: raw=%d, mv=%d, freq=%lu Hz", adc_raw, voltage_mv, new_freq);
        last_log = now;
    }
    
    // Only update if frequency changed significantly (reduce log spam)
    if (abs((int)new_freq - (int)status.tone_freq_hz) > 5) {
        status.tone_freq_hz = new_freq;
        tone_gen_set_frequency(status.tone_freq_hz);
        ESP_LOGI(TAG, "Tone frequency updated to %lu Hz", status.tone_freq_hz);
    }
    
    read_count++;
}

void app_main(void) {
    ESP_LOGI(TAG, "MeshNet Audio TX starting...");
    
    // Initialize control layer
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(buttons_init());
    
    // Initialize network layer (AP mode)
    ESP_ERROR_CHECK(network_init_ap());
    ESP_ERROR_CHECK(network_start_latency_measurement());

    // Initialize ADC for pitch control (GPIO 3 - ADC1_CHANNEL_3 / A2)
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

    // Initialize ADC calibration
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .chan = ADC_CHANNEL_3,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cali_config, &adc1_cali_handle));

    // Initialize audio layer
    ESP_ERROR_CHECK(tone_gen_init(status.tone_freq_hz));
    ESP_ERROR_CHECK(usb_audio_init());
    ESP_ERROR_CHECK(adc_audio_init());
    // Don't start ADC yet - will start when switching to AUX mode
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

    // Create semaphore for timer
    tx_timer_sem = xSemaphoreCreateBinary();
    if (!tx_timer_sem) {
        ESP_LOGE(TAG, "Failed to create timer semaphore");
        return;
    }
    
    // Create and start 1ms periodic timer
    const esp_timer_create_args_t timer_args = {
        .callback = &tx_timer_callback,
        .name = "tx_pacer",
        .dispatch_method = ESP_TIMER_TASK
    };
    esp_timer_handle_t tx_timer;
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &tx_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tx_timer, 1000)); // 1ms = 1000us
    
    ESP_LOGI(TAG, "TX initialized, starting main loop");

    uint32_t bytes_sent = 0;
    uint32_t last_stats_update = xTaskGetTickCount();
    
    while (1) {
        // Wait for 1ms timer tick (but only send every 10ms)
        xSemaphoreTake(tx_timer_sem, portMAX_DELAY);
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
            // Update knob every 1ms for responsiveness, but don't generate audio
            if (status.input_mode == INPUT_MODE_TONE) {
                update_tone_from_adc();
            }
            continue; // Skip non-frame ticks for audio generation
        }
        
        status.audio_active = false;
        switch (status.input_mode) {
        case INPUT_MODE_TONE:
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
                    
                    // Detect actual audio by measuring AC variance (not DC offset)
                    // Calculate mean
                    int64_t sum_left = 0, sum_right = 0;
                    for (size_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
                        sum_left += stereo_frame[i * 2];
                        sum_right += stereo_frame[i * 2 + 1];
                    }
                    int32_t mean_left = (int32_t)(sum_left / AUDIO_FRAME_SAMPLES);
                    int32_t mean_right = (int32_t)(sum_right / AUDIO_FRAME_SAMPLES);
                    
                    // Calculate variance (AC component)
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
                    
                    // Threshold: look for AC variation, not DC offset
                    const int32_t SIGNAL_THRESHOLD = 500;  // ~1.5% of full scale
                    
                    if (std_avg > SIGNAL_THRESHOLD) {
                        status.audio_active = true;
                        if ((tx_seq & 0xFF) == 0) {
                            ESP_LOGI(TAG, "AUX: STD=%ld, DC_L=%ld, DC_R=%ld", std_avg, mean_left, mean_right);
                        }
                    } else {
                        status.audio_active = false;
                        memset(stereo_frame, 0, sizeof(stereo_frame));
                    }
                } else {
                    // No data or error - fill with silence
                    memset(stereo_frame, 0, sizeof(stereo_frame));
                    status.audio_active = false;
                    if (ret != ESP_OK) {
                        ESP_LOGW(TAG, "AUX: ADC read error: %s", esp_err_to_name(ret));
                    }
                }
            }
            break;
        default:
            // Handle any unhandled cases
            break;
        }
        
        // If we have audio and network is ready (STA has IP), send raw PCM
        if (status.audio_active && network_is_stream_ready()) {
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

        esp_err_t send_ret = network_udp_send_audio(framed_buffer, sizeof(hdr) + AUDIO_FRAME_BYTES);
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

        // Update display at 10 Hz (every 100ms) to reduce I2C overhead
        if ((ms_tick % 100) == 0) {
            display_render_tx(current_view, &status);
        }

        // Reset watchdog
        esp_task_wdt_reset();
    }
}
