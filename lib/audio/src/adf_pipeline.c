/**
 * Audio Pipeline Implementation with Opus Codec
 * 
 * EVENT-DRIVEN DESIGN:
 * - Tasks block on notifications instead of polling
 * - Ring buffers notify consumer tasks when data is written
 * - No busy-wait loops - all state changes trigger events
 * 
 * Pipeline flow:
 * TX: I2S → capture_task → [pcm_buffer] → encode_task → mesh
 * RX: mesh → [opus_buffer] → decode_task → [pcm_buffer] → playback_task → I2S
 */

#include "audio/adf_pipeline.h"
#include "audio/es8388_audio.h"
#include "audio/i2s_audio.h"
#include "audio/ring_buffer.h"
#include "audio/tone_gen.h"
#include "audio/usb_audio.h"
#include "network/mesh_net.h"
#include "config/build.h"
#include "config/pins.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_mesh.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <string.h>
#include <arpa/inet.h>
#include <math.h>

#include "opus.h"

// esp-dsp for FFT
#include "esp_dsp.h"
#include "dsps_fft2r.h"
#include "dsps_wind_hann.h"

static const char *TAG = "adf_pipeline";

struct adf_pipeline {
    adf_pipeline_type_t type;
    volatile bool running;
    bool enable_local_output;
    volatile adf_input_mode_t input_mode;  // TX input source
    
    TaskHandle_t capture_task;
    TaskHandle_t encode_task;
    TaskHandle_t decode_task;
    TaskHandle_t playback_task;
    
    OpusEncoder *encoder;
    OpusDecoder *decoder;
    
    ring_buffer_t *pcm_buffer;
    ring_buffer_t *opus_buffer;
    
    SemaphoreHandle_t mutex;
    
    adf_pipeline_stats_t stats;
    uint16_t input_silence_frames;
    
    uint16_t tx_seq;
    uint16_t last_rx_seq;
    bool first_rx_packet;

    float fft_bins[FFT_PORTAL_BIN_COUNT];
    bool fft_valid;
    uint32_t fft_frame_counter;
};

// Buffer and stack sizes are now centralized in config/build.h
// This ensures consistent memory budgeting across all modules

// Static buffers for audio tasks - MOVED OFF STACK to save heap
// These live in .bss instead of being allocated on task stacks
static int16_t s_capture_stereo_frame[AUDIO_FRAME_SAMPLES * 2];  // 7680 bytes
static int16_t s_capture_mono_frame[AUDIO_FRAME_SAMPLES];         // 3840 bytes
static int16_t s_encode_pcm_frame[AUDIO_FRAME_SAMPLES];           // 3840 bytes (encode task)
static uint8_t s_encode_opus_frame[OPUS_MAX_FRAME_BYTES];         // 512 bytes (encode task)
static uint8_t s_encode_packet_buffer[NET_FRAME_HEADER_SIZE + OPUS_MAX_FRAME_BYTES];  // ~528 bytes
static int16_t s_decode_pcm_frame[AUDIO_FRAME_SAMPLES];           // 3840 bytes
static int16_t s_playback_mono_frame[AUDIO_FRAME_SAMPLES];        // 3840 bytes
static int16_t s_playback_stereo_frame[AUDIO_FRAME_SAMPLES * 2];  // 7680 bytes
static int16_t s_playback_silence[AUDIO_FRAME_SAMPLES * 2];       // 7680 bytes (zero-initialized)
static float s_fft_window[FFT_ANALYSIS_SIZE];
static float s_fft_complex[FFT_ANALYSIS_SIZE * 2];  // Interleaved [Re0, Im0, Re1, Im1, ...]
static bool s_fft_initialized = false;
static bool s_fft_init_attempted = false;
static adf_pipeline_handle_t s_latest_pipeline = NULL;

static void tx_capture_task(void *arg);
static void tx_encode_task(void *arg);
static void rx_decode_task(void *arg);
static void rx_playback_task(void *arg);
static esp_err_t init_opus_encoder(adf_pipeline_handle_t pipeline, uint32_t bitrate, uint8_t complexity);
static esp_err_t init_opus_decoder(adf_pipeline_handle_t pipeline);
static uint16_t calculate_pcm_peak(const int16_t *samples, size_t sample_count);
static void tx_update_input_activity(adf_pipeline_handle_t pipeline, bool signal_present, uint16_t peak);
static void fft_init_once(void);
static void fft_process_frame(adf_pipeline_handle_t pipeline, const int16_t *samples, size_t sample_count);

static uint16_t calculate_pcm_peak(const int16_t *samples, size_t sample_count)
{
    uint16_t peak = 0;
    if (!samples || sample_count == 0) return peak;

    for (size_t i = 0; i < sample_count; i++) {
        int32_t sample = samples[i];
        if (sample < 0) sample = -sample;
        if (sample > peak) {
            peak = (uint16_t)sample;
        }
    }
    return peak;
}

static void tx_update_input_activity(adf_pipeline_handle_t pipeline, bool signal_present, uint16_t peak)
{
    if (!pipeline) return;

    pipeline->stats.input_peak = peak;

    if (signal_present) {
        pipeline->input_silence_frames = 0;
        pipeline->stats.input_signal_present = true;
        return;
    }

    if (pipeline->input_silence_frames < AUDIO_INPUT_ACTIVITY_HOLD_FRAMES) {
        pipeline->input_silence_frames++;
        return;
    }

    pipeline->stats.input_signal_present = false;
}

static int s_fft_bar_start[FFT_PORTAL_BIN_COUNT];
static int s_fft_bar_end[FFT_PORTAL_BIN_COUNT];

static void fft_init_once(void)
{
    if (s_fft_init_attempted) {
        return;
    }
    s_fft_init_attempted = true;

#ifdef CONFIG_COMBO_BUILD
    // FFT disabled on COMBO - causes boot loop (likely esp-dsp conflict with USB/ES8388)
    // FFT visualization is on RX nodes anyway
    ESP_LOGW(TAG, "FFT disabled on COMBO build");
    return;
#endif

    ESP_LOGI(TAG, "FFT init: calling dsps_fft2r_init_fc32 (heap=%lu)...",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    
    // Initialize esp-dsp FFT tables
    esp_err_t ret = dsps_fft2r_init_fc32(NULL, CONFIG_DSP_MAX_FFT_SIZE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp-dsp FFT init failed: %d", ret);
        return;
    }

    // Generate Hann window using esp-dsp
    dsps_wind_hann_f32(s_fft_window, FFT_ANALYSIS_SIZE);

    // Pre-compute logarithmic frequency bin mapping
    const float ratio = (float)FFT_MAX_FREQ_HZ / (float)FFT_MIN_FREQ_HZ;
    const int max_bin = (FFT_ANALYSIS_SIZE / 2) - 1;

    for (int i = 0; i < FFT_PORTAL_BIN_COUNT; i++) {
        float f0 = (float)FFT_MIN_FREQ_HZ * powf(ratio, (float)i / (float)FFT_PORTAL_BIN_COUNT);
        float f1 = (float)FFT_MIN_FREQ_HZ * powf(ratio, (float)(i + 1) / (float)FFT_PORTAL_BIN_COUNT);

        int k0 = (int)floorf((f0 * (float)FFT_ANALYSIS_SIZE) / (float)AUDIO_SAMPLE_RATE);
        int k1 = (int)ceilf((f1 * (float)FFT_ANALYSIS_SIZE) / (float)AUDIO_SAMPLE_RATE);

        if (k0 < 1) k0 = 1;
        if (k0 > max_bin) k0 = max_bin;
        if (k1 <= k0) k1 = k0 + 1;
        if (k1 > max_bin + 1) k1 = max_bin + 1;

        s_fft_bar_start[i] = k0;
        s_fft_bar_end[i] = k1;
    }

    s_fft_initialized = true;
    ESP_LOGI(TAG, "FFT init complete: size=%d, bars=%d", FFT_ANALYSIS_SIZE, FFT_PORTAL_BIN_COUNT);
    ESP_LOGI(TAG, "esp-dsp FFT init OK: size=%d, bars=%d", FFT_ANALYSIS_SIZE, FFT_PORTAL_BIN_COUNT);
}

static void fft_process_frame(adf_pipeline_handle_t pipeline, const int16_t *samples, size_t sample_count)
{
    if (!pipeline || !samples || sample_count < FFT_ANALYSIS_SIZE) {
        return;
    }

    pipeline->fft_frame_counter++;
    if ((pipeline->fft_frame_counter % FFT_UPDATE_INTERVAL_FRAMES) != 0) {
        return;
    }

    if (!s_fft_initialized) {
        fft_init_once();
        if (!s_fft_initialized) {
            return;
        }
    }

    // Prepare interleaved complex input [Re0, Im0, Re1, Im1, ...]
    const size_t offset = sample_count - FFT_ANALYSIS_SIZE;
    for (int i = 0; i < FFT_ANALYSIS_SIZE; i++) {
        float normalized = (float)samples[offset + i] / 32768.0f;
        s_fft_complex[i * 2 + 0] = normalized * s_fft_window[i];  // Real (windowed)
        s_fft_complex[i * 2 + 1] = 0.0f;                          // Imaginary (zero for real input)
    }

    // Execute esp-dsp FFT (in-place)
    esp_err_t ret = dsps_fft2r_fc32(s_fft_complex, FFT_ANALYSIS_SIZE);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "FFT compute failed: %d", ret);
        return;
    }

    // Bit-reverse the output
    dsps_bit_rev_fc32(s_fft_complex, FFT_ANALYSIS_SIZE);

    float bins_local[FFT_PORTAL_BIN_COUNT];
    const float db_span = (FFT_DB_CEIL - FFT_DB_FLOOR) > 0.01f ? (FFT_DB_CEIL - FFT_DB_FLOOR) : 1.0f;

    // Map FFT bins to logarithmic display bars
    for (int b = 0; b < FFT_PORTAL_BIN_COUNT; b++) {
        float peak_db = FFT_DB_FLOOR;
        for (int k = s_fft_bar_start[b]; k < s_fft_bar_end[b]; k++) {
            // Extract real and imaginary from interleaved array
            float re = s_fft_complex[k * 2 + 0];
            float im = s_fft_complex[k * 2 + 1];
            float power = (re * re) + (im * im);
            if (power < 1e-12f) power = 1e-12f;

            float db = 10.0f * log10f(power / (float)FFT_ANALYSIS_SIZE);
            if (db > peak_db) {
                peak_db = db;
            }
        }

        float norm = (peak_db - FFT_DB_FLOOR) / db_span;
        if (norm < 0.0f) norm = 0.0f;
        if (norm > 1.0f) norm = 1.0f;
        bins_local[b] = norm;
    }

    if (xSemaphoreTake(pipeline->mutex, 0) == pdTRUE) {
        memcpy(pipeline->fft_bins, bins_local, sizeof(pipeline->fft_bins));
        pipeline->fft_valid = true;
        xSemaphoreGive(pipeline->mutex);
    }
}

adf_pipeline_handle_t adf_pipeline_create(const adf_pipeline_config_t *config)
{
    if (!config) {
        ESP_LOGE(TAG, "NULL config");
        return NULL;
    }
    
    struct adf_pipeline *pipeline = calloc(1, sizeof(struct adf_pipeline));
    if (!pipeline) {
        ESP_LOGE(TAG, "Failed to allocate pipeline");
        return NULL;
    }
    
    pipeline->type = config->type;
    pipeline->enable_local_output = config->enable_local_output;
    pipeline->running = false;
    pipeline->first_rx_packet = true;
    pipeline->input_mode = ADF_INPUT_MODE_AUX;  // Default to line input
    pipeline->fft_valid = false;
    pipeline->fft_frame_counter = 0;
    
    pipeline->mutex = xSemaphoreCreateMutex();
    if (!pipeline->mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        free(pipeline);
        return NULL;
    }
    
    // PCM buffer: stream mode for continuous audio samples
    pipeline->pcm_buffer = ring_buffer_create(PCM_BUFFER_SIZE);
    if (!pipeline->pcm_buffer) {
        ESP_LOGE(TAG, "Failed to create PCM buffer");
        vSemaphoreDelete(pipeline->mutex);
        free(pipeline);
        return NULL;
    }
    
    // Opus buffer: ITEM mode so each [length_prefix + opus_frame] is a discrete item
    // This prevents frame corruption from byte-stream concatenation
    pipeline->opus_buffer = ring_buffer_create_ex(OPUS_BUFFER_SIZE, true);
    if (!pipeline->opus_buffer) {
        ESP_LOGE(TAG, "Failed to create Opus buffer");
        ring_buffer_destroy(pipeline->pcm_buffer);
        vSemaphoreDelete(pipeline->mutex);
        free(pipeline);
        return NULL;
    }
    
    esp_err_t ret;
    if (config->type == ADF_PIPELINE_TX) {
        ret = init_opus_encoder(pipeline, config->opus_bitrate, config->opus_complexity);
    } else {
        ret = init_opus_decoder(pipeline);
    }
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize codec");
        ring_buffer_destroy(pipeline->opus_buffer);
        ring_buffer_destroy(pipeline->pcm_buffer);
        vSemaphoreDelete(pipeline->mutex);
        free(pipeline);
        return NULL;
    }

    // FFT init deferred to first fft_process_frame() call
    // This avoids timing conflicts during early COMBO boot
    
    ESP_LOGI(TAG, "Pipeline created: type=%s, local_output=%d (event-driven)",
             config->type == ADF_PIPELINE_TX ? "TX" : "RX",
             config->enable_local_output);
    s_latest_pipeline = pipeline;
    
    return pipeline;
}

static esp_err_t init_opus_encoder(adf_pipeline_handle_t pipeline, uint32_t bitrate, uint8_t complexity)
{
    int error;
    
    pipeline->encoder = opus_encoder_create(
        AUDIO_SAMPLE_RATE,
        AUDIO_CHANNELS_MONO,
        OPUS_APPLICATION_AUDIO,
        &error
    );
    
    if (error != OPUS_OK || pipeline->encoder == NULL) {
        ESP_LOGE(TAG, "Failed to create Opus encoder: %s", opus_strerror(error));
        return ESP_FAIL;
    }
    
    opus_encoder_ctl(pipeline->encoder, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(pipeline->encoder, OPUS_SET_COMPLEXITY(complexity));
    opus_encoder_ctl(pipeline->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    opus_encoder_ctl(pipeline->encoder, OPUS_SET_VBR(1));
    opus_encoder_ctl(pipeline->encoder, OPUS_SET_VBR_CONSTRAINT(1));
    
    ESP_LOGI(TAG, "Opus encoder initialized: %luHz, %dch, %lubps, complexity=%d",
             (unsigned long)AUDIO_SAMPLE_RATE, AUDIO_CHANNELS_MONO, (unsigned long)bitrate, complexity);
    
    return ESP_OK;
}

static esp_err_t init_opus_decoder(adf_pipeline_handle_t pipeline)
{
    int error;
    
    pipeline->decoder = opus_decoder_create(
        AUDIO_SAMPLE_RATE,
        AUDIO_CHANNELS_MONO,
        &error
    );
    
    if (error != OPUS_OK || pipeline->decoder == NULL) {
        ESP_LOGE(TAG, "Failed to create Opus decoder: %s", opus_strerror(error));
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Opus decoder initialized: %luHz, %dch",
             (unsigned long)AUDIO_SAMPLE_RATE, AUDIO_CHANNELS_MONO);
    
    return ESP_OK;
}

esp_err_t adf_pipeline_start(adf_pipeline_handle_t pipeline)
{
    if (!pipeline) return ESP_ERR_INVALID_ARG;
    
    xSemaphoreTake(pipeline->mutex, portMAX_DELAY);
    
    if (pipeline->running) {
        xSemaphoreGive(pipeline->mutex);
        return ESP_OK;
    }
    
    pipeline->running = true;
    pipeline->tx_seq = 0;
    
    const BaseType_t AUDIO_CORE = 1;
    
    if (pipeline->type == ADF_PIPELINE_TX) {
        BaseType_t ret;
        
        // Create encode task FIRST so we can set it as consumer before capture starts
        ret = xTaskCreatePinnedToCore(tx_encode_task, "adf_enc", ENCODE_TASK_STACK,
                    pipeline, ENCODE_TASK_PRIO, &pipeline->encode_task, AUDIO_CORE);
        if (ret != pdPASS || pipeline->encode_task == NULL) {
            ESP_LOGE(TAG, "Failed to create TX encode task! free=%lu largest=%lu",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            pipeline->running = false;
            xSemaphoreGive(pipeline->mutex);
            return ESP_ERR_NO_MEM;
        }
        
        // Set encode task as consumer of PCM buffer (event-driven)
        ring_buffer_set_consumer(pipeline->pcm_buffer, pipeline->encode_task);
        
        // Now start capture task
        ret = xTaskCreatePinnedToCore(tx_capture_task, "adf_cap", CAPTURE_TASK_STACK, 
                    pipeline, CAPTURE_TASK_PRIO, &pipeline->capture_task, AUDIO_CORE);
        if (ret != pdPASS || pipeline->capture_task == NULL) {
            ESP_LOGE(TAG, "Failed to create TX capture task! free=%lu",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            vTaskDelete(pipeline->encode_task);
            pipeline->encode_task = NULL;
            pipeline->running = false;
            xSemaphoreGive(pipeline->mutex);
            return ESP_ERR_NO_MEM;
        }
        
        ESP_LOGI(TAG, "TX pipeline started on core %d (event-driven)", AUDIO_CORE);
    } else {
        BaseType_t ret;
        
        // Create decode task FIRST (needs to exist before setting as consumer)
        ret = xTaskCreatePinnedToCore(rx_decode_task, "adf_dec", DECODE_TASK_STACK,
                    pipeline, DECODE_TASK_PRIO, &pipeline->decode_task, AUDIO_CORE);
        if (ret != pdPASS || pipeline->decode_task == NULL) {
            ESP_LOGE(TAG, "Failed to create decode task! Free heap: %lu", 
                     (unsigned long)esp_get_free_heap_size());
            xSemaphoreGive(pipeline->mutex);
            return ESP_ERR_NO_MEM;
        }
        
        // Create playback task
        ret = xTaskCreatePinnedToCore(rx_playback_task, "adf_play", PLAYBACK_TASK_STACK,
                    pipeline, PLAYBACK_TASK_PRIO, &pipeline->playback_task, AUDIO_CORE);
        if (ret != pdPASS || pipeline->playback_task == NULL) {
            ESP_LOGE(TAG, "Failed to create playback task!");
            vTaskDelete(pipeline->decode_task);
            pipeline->decode_task = NULL;
            xSemaphoreGive(pipeline->mutex);
            return ESP_ERR_NO_MEM;
        }
        
        // Set up event-driven consumers:
        // - decode_task is notified when mesh writes to opus_buffer
        // - playback_task is notified when decode writes to pcm_buffer
        ring_buffer_set_consumer(pipeline->opus_buffer, pipeline->decode_task);
        ring_buffer_set_consumer(pipeline->pcm_buffer, pipeline->playback_task);
        
        ESP_LOGI(TAG, "RX pipeline started on core %d (event-driven)", AUDIO_CORE);
    }
    
    xSemaphoreGive(pipeline->mutex);
    return ESP_OK;
}

esp_err_t adf_pipeline_stop(adf_pipeline_handle_t pipeline)
{
    if (!pipeline) return ESP_ERR_INVALID_ARG;
    
    xSemaphoreTake(pipeline->mutex, portMAX_DELAY);
    if (!pipeline->running) {
        xSemaphoreGive(pipeline->mutex);
        return ESP_OK;
    }
    pipeline->running = false;
    xSemaphoreGive(pipeline->mutex);
    
    // Notify blocked tasks to wake up and check running flag
    if (pipeline->encode_task) xTaskNotifyGive(pipeline->encode_task);
    if (pipeline->decode_task) xTaskNotifyGive(pipeline->decode_task);
    if (pipeline->playback_task) xTaskNotifyGive(pipeline->playback_task);
    
    vTaskDelay(pdMS_TO_TICKS(100));
    
    if (pipeline->capture_task) {
        vTaskDelete(pipeline->capture_task);
        pipeline->capture_task = NULL;
    }
    if (pipeline->encode_task) {
        vTaskDelete(pipeline->encode_task);
        pipeline->encode_task = NULL;
    }
    if (pipeline->decode_task) {
        vTaskDelete(pipeline->decode_task);
        pipeline->decode_task = NULL;
    }
    if (pipeline->playback_task) {
        vTaskDelete(pipeline->playback_task);
        pipeline->playback_task = NULL;
    }
    
    ESP_LOGI(TAG, "Pipeline stopped");
    return ESP_OK;
}

void adf_pipeline_destroy(adf_pipeline_handle_t pipeline)
{
    if (!pipeline) return;
    
    adf_pipeline_stop(pipeline);
    
    if (pipeline->encoder) {
        opus_encoder_destroy(pipeline->encoder);
    }
    if (pipeline->decoder) {
        opus_decoder_destroy(pipeline->decoder);
    }
    if (pipeline->pcm_buffer) {
        ring_buffer_destroy(pipeline->pcm_buffer);
    }
    if (pipeline->opus_buffer) {
        ring_buffer_destroy(pipeline->opus_buffer);
    }
    if (pipeline->mutex) {
        vSemaphoreDelete(pipeline->mutex);
    }
    
    free(pipeline);
    if (s_latest_pipeline == pipeline) {
        s_latest_pipeline = NULL;
    }
    ESP_LOGI(TAG, "Pipeline destroyed");
}

bool adf_pipeline_is_running(adf_pipeline_handle_t pipeline)
{
    if (!pipeline) return false;
    return pipeline->running;
}

/**
 * TX Capture Task (event-driven)
 * 
 * Reads audio based on current input mode:
 * - AUX: ES8388 I2S line input
 * - TONE: Tone generator (for testing)
 * - USB: USB audio input (future)
 * 
 * Writes to PCM buffer which notifies encode task automatically.
 */
static void tx_capture_task(void *arg)
{
    adf_pipeline_handle_t pipeline = (adf_pipeline_handle_t)arg;
    // Use static buffers instead of stack allocation (saves ~11KB stack)
    int16_t *stereo_frame = s_capture_stereo_frame;
    int16_t *mono_frame = s_capture_mono_frame;
    
    ESP_LOGI(TAG, "TX capture task started (mode-aware), stack=%u", 
             uxTaskGetStackHighWaterMark(NULL));
    
    static uint32_t no_data_count = 0;
    static uint32_t local_output_count = 0;
    
    while (pipeline->running) {
        size_t frames_read = 0;
        esp_err_t ret = ESP_OK;
        
        adf_input_mode_t mode = pipeline->input_mode;
        
        switch (mode) {
            case ADF_INPUT_MODE_TONE:
                tone_gen_fill_buffer(mono_frame, AUDIO_FRAME_SAMPLES);
                frames_read = AUDIO_FRAME_SAMPLES;
                tx_update_input_activity(pipeline, true, 16000);
                fft_process_frame(pipeline, mono_frame, frames_read);
                
                if (pipeline->enable_local_output) {
                    for (size_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
                        stereo_frame[i * 2] = mono_frame[i];
                        stereo_frame[i * 2 + 1] = mono_frame[i];
                    }
                    es8388_audio_write_stereo(stereo_frame, frames_read);
                    local_output_count++;
                    if ((local_output_count % 1000) == 0) {
                        ESP_LOGI(TAG, "Local output: %lu frames, mode=TONE", local_output_count);
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(AUDIO_FRAME_MS));
                break;
                
            case ADF_INPUT_MODE_USB:
                tx_update_input_activity(pipeline, usb_audio_is_active(), 0);
                vTaskDelay(pdMS_TO_TICKS(AUDIO_FRAME_MS));
                continue;
                
            case ADF_INPUT_MODE_AUX:
            default: {
#define TX_TEST_TONE_MODE 0  // Set to 1 to bypass ES8388 and send pure tone
#if TX_TEST_TONE_MODE
                // Generate a 440Hz test tone to bypass ES8388 capture
                static uint32_t tone_sample_offset = 0;
                const float freq = 440.0f;
                const float amplitude = 16000.0f;  // ~50% of max
                for (size_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
                    float t = (float)(tone_sample_offset + i) / (float)AUDIO_SAMPLE_RATE;
                    mono_frame[i] = (int16_t)(amplitude * sinf(2.0f * M_PI * freq * t));
                }
                tone_sample_offset += AUDIO_FRAME_SAMPLES;
                frames_read = AUDIO_FRAME_SAMPLES;
                
                static bool tone_log_once = true;
                if (tone_log_once) {
                    tone_log_once = false;
                    ESP_LOGW(TAG, "*** TX TEST TONE MODE - bypassing ES8388 ***");
                }
                vTaskDelay(pdMS_TO_TICKS(AUDIO_FRAME_MS));
                break;
#endif
                ret = es8388_audio_read_stereo(stereo_frame, AUDIO_FRAME_SAMPLES, &frames_read);
                
                if (ret != ESP_OK || frames_read == 0) {
                    tx_update_input_activity(pipeline, false, 0);
                    no_data_count++;
                    if ((no_data_count % 100) == 0) {
                        ESP_LOGW(TAG, "I2S read: ret=%d, frames=%u, no_data=%lu", 
                                 ret, frames_read, no_data_count);
                    }
                    vTaskDelay(1);
                    continue;
                }
                no_data_count = 0;
                
                if (frames_read < AUDIO_FRAME_SAMPLES) {
                    memset(stereo_frame + (frames_read * 2), 0, 
                           (AUDIO_FRAME_SAMPLES - frames_read) * 2 * sizeof(int16_t));
                    frames_read = AUDIO_FRAME_SAMPLES;
                }
                
                for (size_t i = 0; i < frames_read; i++) {
                    mono_frame[i] = (stereo_frame[i * 2] + stereo_frame[i * 2 + 1]) / 2;
                }

                uint16_t peak = calculate_pcm_peak(mono_frame, frames_read);
                tx_update_input_activity(pipeline, peak >= AUDIO_INPUT_ACTIVITY_PEAK_THRESHOLD, peak);
                fft_process_frame(pipeline, mono_frame, frames_read);
                
                static uint32_t capture_count = 0;
                capture_count++;
                if (capture_count <= 5 || (capture_count % 1000) == 0) {
                    ESP_LOGI(TAG, "Capture #%lu: stereo[0]=%d stereo[1]=%d mono[0]=%d mono[100]=%d",
                             capture_count, (int)stereo_frame[0], (int)stereo_frame[1],
                             (int)mono_frame[0], (int)mono_frame[100]);
                }
                
                if (pipeline->enable_local_output) {
                    es8388_audio_write_stereo(stereo_frame, frames_read);
                    local_output_count++;
                    if ((local_output_count % 1000) == 0) {
                        ESP_LOGI(TAG, "Local output: %lu frames, mode=AUX", local_output_count);
                    }
                }
                break;
            }
        }
        
        ret = ring_buffer_write(pipeline->pcm_buffer, (uint8_t *)mono_frame, 
                                frames_read * sizeof(int16_t));
        if (ret != ESP_OK) {
            pipeline->stats.frames_dropped++;
        }
    }
    
    ESP_LOGI(TAG, "TX capture task exiting");
    while (1) { vTaskDelay(portMAX_DELAY); }
}

/**
 * TX Encode Task (event-driven)
 * 
 * Blocks waiting for notification from PCM buffer.
 * When notified, reads and encodes a frame, then sends to mesh.
 */
// Batch buffer: accumulate MESH_FRAMES_PER_PACKET Opus frames before sending
// Layout: [header][len1_hi][len1_lo][opus1...][len2_hi][len2_lo][opus2...]
static uint8_t s_batch_buffer[NET_FRAME_HEADER_SIZE + MESH_FRAMES_PER_PACKET * (2 + OPUS_MAX_FRAME_BYTES)];

static void tx_encode_task(void *arg)
{
    adf_pipeline_handle_t pipeline = (adf_pipeline_handle_t)arg;
    int16_t *pcm_frame = s_encode_pcm_frame;
    uint8_t *opus_frame = s_encode_opus_frame;
    
    uint8_t batch_count = 0;
    size_t batch_payload_len = 0;
    
    ESP_LOGI(TAG, "TX encode task started (batch=%d), stack=%u",
             MESH_FRAMES_PER_PACKET, uxTaskGetStackHighWaterMark(NULL));
    
    while (pipeline->running) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        
        if (!pipeline->running) break;
        
        while (ring_buffer_available(pipeline->pcm_buffer) >= AUDIO_FRAME_BYTES) {
            esp_err_t ret = ring_buffer_read(pipeline->pcm_buffer, (uint8_t *)pcm_frame, AUDIO_FRAME_BYTES);
            if (ret != ESP_OK) break;

            bool signal_present = pipeline->stats.input_signal_present;
#if !TX_CONTINUOUS_STREAMING
            if (pipeline->input_mode != ADF_INPUT_MODE_TONE && !signal_present) {
                // No real input activity: consume PCM but skip encode/send to keep TX idle.
                // Also clear any partial batch so stale pre-silence frames don't flush later.
                batch_count = 0;
                batch_payload_len = 0;
                continue;
            }
#else
            (void)signal_present;
#endif
            
            int64_t start_us = esp_timer_get_time();
            
            int opus_len = opus_encode(
                pipeline->encoder,
                pcm_frame,
                AUDIO_FRAME_SAMPLES,
                opus_frame,
                OPUS_MAX_FRAME_BYTES
            );
            
            int64_t encode_time = esp_timer_get_time() - start_us;
            pipeline->stats.avg_encode_time_us = 
                (pipeline->stats.avg_encode_time_us * 7 + (uint32_t)encode_time) / 8;
            
            if (opus_len < 0) {
                ESP_LOGW(TAG, "Opus encode failed: %s", opus_strerror(opus_len));
                continue;
            }
            
            // Append to batch: [len_hi][len_lo][opus_data...]
            uint8_t *dst = s_batch_buffer + NET_FRAME_HEADER_SIZE + batch_payload_len;
            dst[0] = (opus_len >> 8) & 0xFF;
            dst[1] = opus_len & 0xFF;
            memcpy(dst + 2, opus_frame, opus_len);
            batch_payload_len += 2 + opus_len;
            batch_count++;
            
            // Send when batch is full
            if (batch_count >= MESH_FRAMES_PER_PACKET) {
                net_frame_header_t *hdr = (net_frame_header_t *)s_batch_buffer;
                hdr->magic = NET_FRAME_MAGIC;
                hdr->version = NET_FRAME_VERSION;
                hdr->type = NET_PKT_TYPE_AUDIO_OPUS;
                hdr->stream_id = 1;
                hdr->seq = htons(pipeline->tx_seq);
                hdr->timestamp = htonl((uint32_t)(esp_timer_get_time() / 1000));
                hdr->payload_len = htons((uint16_t)batch_payload_len);
                hdr->ttl = 6;
                hdr->frame_count = batch_count;
                memcpy(hdr->src_id, network_get_src_id(), NETWORK_SRC_ID_LEN);
                
                ret = network_send_audio(s_batch_buffer, NET_FRAME_HEADER_SIZE + batch_payload_len);
                if (ret == ESP_OK) {
                    pipeline->stats.frames_processed += batch_count;
                } else if (ret != ESP_ERR_MESH_DISCONNECTED && ret != ESP_ERR_INVALID_STATE) {
                    pipeline->stats.frames_dropped += batch_count;
                }
                
                pipeline->tx_seq += batch_count;
                
                if ((pipeline->tx_seq & 0xFF) == 0) {
                    ESP_LOGI(TAG, "TX: seq=%u, batch=%u, payload=%u, enc=%luus",
                             pipeline->tx_seq, batch_count, (unsigned)batch_payload_len,
                             (unsigned long)pipeline->stats.avg_encode_time_us);
                }
                
                batch_count = 0;
                batch_payload_len = 0;
            }
        }
    }
    
    ESP_LOGI(TAG, "TX encode task exiting");
    while (1) { vTaskDelay(portMAX_DELAY); }
}

/**
 * Feed Opus data from mesh callback to RX pipeline.
 * 
 * Called from mesh RX task - writes to opus_buffer which
 * automatically notifies the decode task.
 */
esp_err_t adf_pipeline_feed_opus(adf_pipeline_handle_t pipeline,
                                  const uint8_t *opus_data, size_t opus_len,
                                  uint16_t seq, uint32_t timestamp)
{
    if (!pipeline || pipeline->type != ADF_PIPELINE_RX) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (opus_len > OPUS_MAX_FRAME_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Sequence gap detection
    if (!pipeline->first_rx_packet) {
        uint16_t expected = (pipeline->last_rx_seq + 1) & 0xFFFF;
        if (seq != expected) {
            int gap = (int16_t)(seq - expected);
            if (gap > 0 && gap < 100) {
                pipeline->stats.frames_dropped += gap;
            }
        }
    }
    pipeline->first_rx_packet = false;
    pipeline->last_rx_seq = seq;
    
    // Pre-check capacity
    size_t needed = 2 + opus_len;
    size_t available_space = OPUS_BUFFER_SIZE - ring_buffer_available(pipeline->opus_buffer);
    if (available_space < needed) {
        pipeline->stats.frames_dropped++;
        return ESP_ERR_NO_MEM;
    }
    
    // Atomic write: length prefix + payload
    uint8_t tmp[2 + OPUS_MAX_FRAME_BYTES];
    tmp[0] = (opus_len >> 8) & 0xFF;
    tmp[1] = opus_len & 0xFF;
    memcpy(&tmp[2], opus_data, opus_len);
    
    // Write notifies decode task automatically
    esp_err_t ret = ring_buffer_write(pipeline->opus_buffer, tmp, 2 + opus_len);
    if (ret != ESP_OK) {
        pipeline->stats.frames_dropped++;
        return ESP_ERR_NO_MEM;
    }
    
    return ESP_OK;
}

/**
 * RX Decode Task (event-driven)
 * 
 * Blocks waiting for notification from opus_buffer (mesh callback).
 * Decodes Opus to PCM and writes to pcm_buffer (notifies playback).
 * 
 * IMPORTANT: FreeRTOS ringbuffer is item-based. Each xRingbufferSend() creates
 * one item containing [length_prefix + payload]. We must receive the whole item
 * at once, not try to read length and payload separately.
 */
static void rx_decode_task(void *arg)
{
    adf_pipeline_handle_t pipeline = (adf_pipeline_handle_t)arg;
    // Use static buffer instead of stack allocation (saves ~4KB stack)
    int16_t *pcm_frame = s_decode_pcm_frame;
    
    ESP_LOGI(TAG, "RX decode task started (event-driven, item-based)");
    
    while (pipeline->running) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        
        if (!pipeline->running) break;
        
        size_t item_size = 0;
        uint8_t *item;
        
        while ((item = ring_buffer_receive_item(pipeline->opus_buffer, &item_size)) != NULL) {
            if (item_size < 2) {
                ESP_LOGW(TAG, "Opus item too small: %u", (unsigned)item_size);
                ring_buffer_return_item(pipeline->opus_buffer, item);
                continue;
            }
            
            uint16_t opus_len = (item[0] << 8) | item[1];
            if (opus_len + 2 > item_size || opus_len > OPUS_MAX_FRAME_BYTES) {
                ESP_LOGW(TAG, "Invalid opus_len=%u for item_size=%u", opus_len, (unsigned)item_size);
                ring_buffer_return_item(pipeline->opus_buffer, item);
                continue;
            }
            
            int64_t start_us = esp_timer_get_time();
            
            // Save first bytes for debug before decode (in case it fails)
            // Guard against small packets
            uint8_t first_bytes[4] = {0};
            if (opus_len >= 4) {
                first_bytes[0] = item[2];
                first_bytes[1] = item[3];
                first_bytes[2] = item[4];
                first_bytes[3] = item[5];
            }
            
            int samples_decoded = opus_decode(
                pipeline->decoder,
                &item[2],
                opus_len,
                pcm_frame,
                AUDIO_FRAME_SAMPLES,
                0
            );
            
            ring_buffer_return_item(pipeline->opus_buffer, item);
            
            int64_t decode_time = esp_timer_get_time() - start_us;
            pipeline->stats.avg_decode_time_us = 
                (pipeline->stats.avg_decode_time_us * 7 + (uint32_t)decode_time) / 8;
            
            if (samples_decoded < 0) {
                static uint32_t decode_error_count = 0;
                decode_error_count++;
                if ((decode_error_count % 500) == 1) {
                    ESP_LOGW(TAG, "Opus decode failed: %s (item_size=%u, opus_len=%u, first_bytes=%02x%02x%02x%02x, errors=%lu)",
                             opus_strerror(samples_decoded), (unsigned)item_size, opus_len,
                             first_bytes[0], first_bytes[1], first_bytes[2], first_bytes[3], decode_error_count);
                }
                continue;
            }

            fft_process_frame(pipeline, pcm_frame, (size_t)samples_decoded);
            
            // Debug: log first decoded frame to verify Opus output
            static bool first_pcm_log = true;
            if (first_pcm_log && samples_decoded > 0) {
                first_pcm_log = false;
                ESP_LOGI(TAG, "First decoded frame: samples=%d, s[0]=%d, s[1]=%d, s[2]=%d, s[3]=%d",
                         samples_decoded, (int)pcm_frame[0], (int)pcm_frame[1],
                         (int)pcm_frame[2], (int)pcm_frame[3]);
            }
            
            size_t pcm_bytes = samples_decoded * sizeof(int16_t);
            esp_err_t ret = ring_buffer_write(pipeline->pcm_buffer, (uint8_t *)pcm_frame, pcm_bytes);
            if (ret != ESP_OK) {
                pipeline->stats.frames_dropped++;
            } else {
                pipeline->stats.frames_processed++;
            }
        }
    }
    
    ESP_LOGI(TAG, "RX decode task exiting");
    while (1) { vTaskDelay(portMAX_DELAY); }
}

/**
 * RX Playback Task (event-driven)
 * 
 * Blocks waiting for notification from pcm_buffer (decode task).
 * I2S write is naturally blocking, providing output pacing.
 */
// Set to 1 to test I2S output with pure tone (bypasses entire pipeline)
#define RX_TEST_TONE_MODE 0

static void rx_playback_task(void *arg)
{
    adf_pipeline_handle_t pipeline = (adf_pipeline_handle_t)arg;
    // Use static buffers instead of stack allocation (saves ~19KB stack!)
    int16_t *mono_frame = s_playback_mono_frame;
    int16_t *stereo_frame = s_playback_stereo_frame;
    int16_t *silence = s_playback_silence;  // Already zero-initialized in .bss
    
    bool prefilled = false;
    size_t prefill_bytes = AUDIO_FRAME_BYTES * JITTER_PREFILL_FRAMES;  // Initial prefill
    
    ESP_LOGI(TAG, "RX playback task started (event-driven), stack=%u",
             uxTaskGetStackHighWaterMark(NULL));

#if RX_TEST_TONE_MODE
    ESP_LOGW(TAG, "*** TONE TEST MODE - bypassing audio pipeline ***");
    static int16_t tone_buffer[AUDIO_FRAME_SAMPLES];
    const float freq = 440.0f;
    const float amplitude = 8000.0f;
    uint32_t sample_offset = 0;
    
    while (pipeline->running) {
        for (size_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
            float t = (float)(sample_offset + i) / (float)AUDIO_SAMPLE_RATE;
            tone_buffer[i] = (int16_t)(amplitude * sinf(2.0f * M_PI * freq * t));
        }
        sample_offset += AUDIO_FRAME_SAMPLES;
        
#if defined(CONFIG_USE_ES8388)
        for (size_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
            stereo_frame[2 * i]     = tone_buffer[i];
            stereo_frame[2 * i + 1] = tone_buffer[i];
        }
        es8388_audio_write_stereo(stereo_frame, AUDIO_FRAME_SAMPLES);
#else
        i2s_audio_write_mono_as_stereo(tone_buffer, AUDIO_FRAME_SAMPLES);
#endif
        taskYIELD();
    }
    ESP_LOGI(TAG, "Tone test task exiting");
    while (1) { vTaskDelay(portMAX_DELAY); }
#endif
    
    while (pipeline->running) {
        // Block waiting for notification from decode task
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
        
        if (!pipeline->running) break;
        
        size_t available = ring_buffer_available(pipeline->pcm_buffer);
        
        // Wait for prefill
        if (!prefilled) {
            if (available >= prefill_bytes) {
                prefilled = true;
                static uint32_t prefill_count = 0;
                prefill_count++;
                if (prefill_count <= 3 || (prefill_count % 50) == 0) {
                    ESP_LOGI(TAG, "Playback prefilled #%lu (%zu bytes)", prefill_count, available);
                }
            } else {
#if defined(CONFIG_USE_ES8388)
                es8388_audio_write_stereo(silence, AUDIO_FRAME_SAMPLES);
#else
                static int16_t prefill_silence[AUDIO_FRAME_SAMPLES] = {0};
                i2s_audio_write_mono_as_stereo(prefill_silence, AUDIO_FRAME_SAMPLES);
#endif
                continue;
            }
        }
        
        // Consume up to 2 frames per wake-up to catch up if notifications stacked
        // (ulTaskNotifyTake returns after first notification, but more may have arrived)
        int frames_played = 0;
        const int max_frames_per_wake = 2;
        
        while (frames_played < max_frames_per_wake && 
               ring_buffer_available(pipeline->pcm_buffer) >= AUDIO_FRAME_BYTES) {
            esp_err_t ret = ring_buffer_read(pipeline->pcm_buffer, (uint8_t *)mono_frame, AUDIO_FRAME_BYTES);
            if (ret == ESP_OK) {
                static uint32_t playback_count = 0;
                playback_count++;
                
                for (size_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
                    int32_t scaled = (int32_t)(mono_frame[i] * RX_OUTPUT_VOLUME);
                    if (scaled > 32767) scaled = 32767;
                    else if (scaled < -32768) scaled = -32768;
                    mono_frame[i] = (int16_t)scaled;
                }
                
                if (playback_count <= 5 || (playback_count % 500) == 0) {
                    ESP_LOGI(TAG, "Playback #%lu: s[0]=%d s[100]=%d",
                             playback_count, (int)mono_frame[0], (int)mono_frame[100]);
                }
                
#if defined(CONFIG_USE_ES8388)
                for (size_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
                    stereo_frame[2 * i]     = mono_frame[i];
                    stereo_frame[2 * i + 1] = mono_frame[i];
                }
                es8388_audio_write_stereo(stereo_frame, AUDIO_FRAME_SAMPLES);
#else
                i2s_audio_write_mono_as_stereo(mono_frame, AUDIO_FRAME_SAMPLES);
#endif
                frames_played++;
            } else {
                break;
            }
        }
        
        // Check for underrun only if we played nothing
        if (frames_played == 0 && prefilled) {
            pipeline->stats.buffer_underruns++;
            prefilled = false;
            
            // Recalculate prefill based on current network conditions
            uint8_t dynamic_frames = network_get_jitter_prefill_frames();
            prefill_bytes = AUDIO_FRAME_BYTES * dynamic_frames;
            
            static uint32_t underrun_count = 0;
            underrun_count++;
            if (underrun_count <= 5 || (underrun_count % 20) == 0) {
                ESP_LOGW(TAG, "Underrun #%lu - new prefill: %d frames (%zu bytes)", 
                         underrun_count, dynamic_frames, prefill_bytes);
            }
            
#if defined(CONFIG_USE_ES8388)
            es8388_audio_write_stereo(silence, AUDIO_FRAME_SAMPLES);
#else
            static int16_t silence_mono[AUDIO_FRAME_SAMPLES] = {0};
            i2s_audio_write_mono_as_stereo(silence_mono, AUDIO_FRAME_SAMPLES);
#endif
        }
        
        pipeline->stats.buffer_fill_percent = 
            (ring_buffer_available(pipeline->pcm_buffer) * 100) / PCM_BUFFER_SIZE;
    }
    
    ESP_LOGI(TAG, "RX playback task exiting");
    while (1) { vTaskDelay(portMAX_DELAY); }
}

esp_err_t adf_pipeline_get_stats(adf_pipeline_handle_t pipeline, adf_pipeline_stats_t *stats)
{
    if (!pipeline || !stats) return ESP_ERR_INVALID_ARG;
    
    xSemaphoreTake(pipeline->mutex, portMAX_DELAY);
    memcpy(stats, &pipeline->stats, sizeof(adf_pipeline_stats_t));
    xSemaphoreGive(pipeline->mutex);
    
    return ESP_OK;
}

esp_err_t adf_pipeline_set_input_mode(adf_pipeline_handle_t pipeline, adf_input_mode_t mode)
{
    if (!pipeline) return ESP_ERR_INVALID_ARG;
    if (pipeline->type != ADF_PIPELINE_TX) return ESP_ERR_INVALID_STATE;
    
    pipeline->input_mode = mode;
    ESP_LOGI(TAG, "Input mode set to %d", mode);
    
    return ESP_OK;
}

esp_err_t adf_pipeline_get_fft_bins(adf_pipeline_handle_t pipeline,
                                    float *bins_out,
                                    size_t bin_count,
                                    bool *valid_out)
{
    if (!pipeline || !bins_out || bin_count < FFT_PORTAL_BIN_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(pipeline->mutex, portMAX_DELAY);
    memcpy(bins_out, pipeline->fft_bins, sizeof(float) * FFT_PORTAL_BIN_COUNT);
    if (valid_out) {
        *valid_out = pipeline->fft_valid;
    }
    xSemaphoreGive(pipeline->mutex);

    return ESP_OK;
}

esp_err_t adf_pipeline_get_latest_fft_bins(float *bins_out, size_t bin_count, bool *valid_out)
{
    if (!s_latest_pipeline) {
        if (valid_out) {
            *valid_out = false;
        }
        return ESP_ERR_NOT_FOUND;
    }
    return adf_pipeline_get_fft_bins(s_latest_pipeline, bins_out, bin_count, valid_out);
}
