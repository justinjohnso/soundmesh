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
#include "audio/tone_gen.h"
#include "audio/usb_audio.h"
#include "audio/adc_audio.h"
#include "audio/i2s_audio.h"  // Added for UDA1334 output
#include "audio/ring_buffer.h"

static const char *TAG = "combo_main";

// Timer for 1ms pacing
static SemaphoreHandle_t combo_timer_sem = NULL;
static uint32_t ms_tick = 0;

static void combo_timer_callback(void* arg) {
    xSemaphoreGive(combo_timer_sem);
}

static combo_status_t status = {
.input_mode = INPUT_MODE_AUX,
.audio_active = false,
.tone_freq_hz = 110,
.output_volume = 1.0f
};

static display_view_t current_view = DISPLAY_VIEW_AUDIO;  // Default to audio view
static ring_buffer_t *audio_buffer = NULL;
static adc_oneshot_unit_handle_t adc1_handle;
static adc_cali_handle_t adc1_cali_handle = NULL;

// Global audio buffers to reduce stack usage
static int16_t mono_frame[AUDIO_FRAME_SAMPLES];
static int16_t stereo_frame[AUDIO_FRAME_SAMPLES * 2];

// ADC processing function
void update_volume_from_adc(void) {
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

    // Map voltage (0-2200mV) to volume (0.0-1.0)
    float new_volume = (float)voltage_mv / 2200.0f;
    if (new_volume > 1.0f) new_volume = 1.0f;

    // Log periodically
    uint32_t now = xTaskGetTickCount();
    if ((now - last_log) > pdMS_TO_TICKS(2000)) {
        ESP_LOGI(TAG, "Knob: raw=%d, mv=%d, volume=%.2f", adc_raw, voltage_mv, new_volume);
        last_log = now;
    }

    // Update volume
    status.output_volume = new_volume;

    read_count++;
}

void app_main(void) {
    ESP_LOGI(TAG, "MeshNet Audio COMBO starting...");

    // Initialize control layer
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(buttons_init());

    // Initialize ADC for volume control (GPIO 3 - ADC1_CHANNEL_3 / A3)
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_6,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_3, &config));

    // Initialize ADC calibration
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .chan = ADC_CHANNEL_3,
        .atten = ADC_ATTEN_DB_6,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cali_config, &adc1_cali_handle));

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

    ESP_LOGI(TAG, "COMBO initialized, starting main loop");

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
                status.input_mode = (status.input_mode + 1) % 3;

                ESP_LOGI(TAG, "Input mode changed to %d", status.input_mode);
            }
        }

        // Generate/capture audio based on mode (every 10ms to match frame size)
        if ((ms_tick % AUDIO_FRAME_MS) != 0) {
            // Update knob every 1ms for responsiveness, but don't generate audio
            update_volume_from_adc();
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
                        status.audio_active = true;
                    } else {
                        status.audio_active = false;
                        memset(stereo_frame, 0, sizeof(stereo_frame));
                    }
                } else {
                    memset(stereo_frame, 0, sizeof(stereo_frame));
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

        // Update display at 10 Hz (every 100ms)
        if ((ms_tick % 100) == 0) {
            display_render_combo(current_view, &status);
        }

        // Reset watchdog
        esp_task_wdt_reset();
    }
}
