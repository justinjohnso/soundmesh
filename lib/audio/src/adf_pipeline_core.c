#include "adf_pipeline_internal.h"

#include "audio/es8388_audio.h"
#include "audio/i2s_audio.h"
#include "network/mesh_net.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <math.h>
#include <string.h>

static const char *TAG = "adf_pipeline";

int16_t s_capture_stereo_frame[AUDIO_FRAME_SAMPLES * 2];
int16_t s_capture_mono_frame[AUDIO_FRAME_SAMPLES];
int16_t s_encode_pcm_frame[AUDIO_FRAME_SAMPLES];
uint8_t s_encode_opus_frame[OPUS_MAX_FRAME_BYTES];
int16_t s_decode_pcm_frame[AUDIO_FRAME_SAMPLES];
int16_t s_playback_mono_frame[AUDIO_FRAME_SAMPLES];
int16_t s_playback_last_good_mono[AUDIO_FRAME_SAMPLES];
int16_t s_playback_stereo_frame[AUDIO_FRAME_SAMPLES * 2];
int16_t s_playback_silence[AUDIO_FRAME_SAMPLES * 2];

static adf_pipeline_handle_t s_latest_pipeline = NULL;

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
    opus_encoder_ctl(pipeline->encoder, OPUS_SET_INBAND_FEC(OPUS_ENABLE_INBAND_FEC));
    opus_encoder_ctl(pipeline->encoder, OPUS_SET_PACKET_LOSS_PERC(OPUS_EXPECTED_LOSS_PCT));

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

    opus_decoder_ctl(pipeline->decoder, OPUS_SET_GAIN(0));
    pipeline->pending_fec_recovery = false;

    ESP_LOGI(TAG, "Opus decoder initialized: %luHz, %dch",
             (unsigned long)AUDIO_SAMPLE_RATE, AUDIO_CHANNELS_MONO);

    return ESP_OK;
}

esp_err_t adf_pipeline_create_impl(const adf_pipeline_config_t *config, adf_pipeline_handle_t *out_pipeline)
{
    if (!config || !out_pipeline) {
        ESP_LOGE(TAG, "NULL config/out handle");
        return ESP_ERR_INVALID_ARG;
    }

    struct adf_pipeline *pipeline = calloc(1, sizeof(struct adf_pipeline));
    if (!pipeline) {
        ESP_LOGE(TAG, "Failed to allocate pipeline");
        return ESP_ERR_NO_MEM;
    }

    pipeline->type = config->type;
    pipeline->enable_local_output = config->enable_local_output;
    pipeline->running = false;
    pipeline->first_rx_packet = true;
    pipeline->input_mode = ADF_INPUT_MODE_AUX;
    pipeline->fft_valid = false;
    pipeline->fft_frame_counter = 0;
    pipeline->output_gain_linear = RX_OUTPUT_VOLUME;
    pipeline->output_mute = false;
    pipeline->input_gain_linear = 1.0f;
    pipeline->input_mute = false;

    pipeline->mutex = xSemaphoreCreateMutex();
    if (!pipeline->mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        free(pipeline);
        return ESP_ERR_NO_MEM;
    }

    pipeline->pcm_buffer = ring_buffer_create(PCM_BUFFER_SIZE);
    if (!pipeline->pcm_buffer) {
        ESP_LOGE(TAG, "Failed to create PCM buffer");
        vSemaphoreDelete(pipeline->mutex);
        free(pipeline);
        return ESP_ERR_NO_MEM;
    }

    pipeline->opus_buffer = ring_buffer_create_ex(OPUS_BUFFER_SIZE, true);
    if (!pipeline->opus_buffer) {
        ESP_LOGE(TAG, "Failed to create Opus buffer");
        ring_buffer_destroy(pipeline->pcm_buffer);
        vSemaphoreDelete(pipeline->mutex);
        free(pipeline);
        return ESP_ERR_NO_MEM;
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
        return ret;
    }

    ESP_LOGI(TAG, "Pipeline created: type=%s, local_output=%d (event-driven)",
             config->type == ADF_PIPELINE_TX ? "TX" : "RX",
             config->enable_local_output);
    s_latest_pipeline = pipeline;
    *out_pipeline = pipeline;

    return ESP_OK;
}

esp_err_t adf_pipeline_start_impl(adf_pipeline_handle_t pipeline)
{
    if (!pipeline) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(pipeline->mutex, portMAX_DELAY);

    if (pipeline->running) {
        xSemaphoreGive(pipeline->mutex);
        return ESP_OK;
    }

    // Pre-flight memory check before allocating task stacks
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t required_stack = (pipeline->type == ADF_PIPELINE_TX)
        ? (ENCODE_TASK_STACK_BYTES + CAPTURE_TASK_STACK_BYTES)
        : (DECODE_TASK_STACK_BYTES + PLAYBACK_TASK_STACK_BYTES);
    
    if (largest_block < required_stack) {
        ESP_LOGW(TAG, "Heap fragmented: largest_block=%lu < required_stack=%lu (free=%lu)",
                 (unsigned long)largest_block, (unsigned long)required_stack, (unsigned long)free_heap);
    }
    if (free_heap < required_stack + MIN_FREE_HEAP_BYTES) {
        ESP_LOGW(TAG, "Low heap: free=%lu < required=%lu + reserve=%u",
                 (unsigned long)free_heap, (unsigned long)required_stack, MIN_FREE_HEAP_BYTES);
    }

    pipeline->running = true;
    pipeline->tx_seq = 0;

    const BaseType_t AUDIO_CORE = 1;

    if (pipeline->type == ADF_PIPELINE_TX) {
        BaseType_t ret;

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

        ring_buffer_set_consumer(pipeline->pcm_buffer, pipeline->encode_task);

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

        ret = xTaskCreatePinnedToCore(rx_decode_task, "adf_dec", DECODE_TASK_STACK,
                                      pipeline, DECODE_TASK_PRIO, &pipeline->decode_task, AUDIO_CORE);
        if (ret != pdPASS || pipeline->decode_task == NULL) {
            ESP_LOGE(TAG, "Failed to create decode task! Free heap: %lu",
                     (unsigned long)esp_get_free_heap_size());
            pipeline->running = false;
            xSemaphoreGive(pipeline->mutex);
            return ESP_ERR_NO_MEM;
        }

        ret = xTaskCreatePinnedToCore(rx_playback_task, "adf_play", PLAYBACK_TASK_STACK,
                                      pipeline, PLAYBACK_TASK_PRIO, &pipeline->playback_task, AUDIO_CORE);
        if (ret != pdPASS || pipeline->playback_task == NULL) {
            ESP_LOGE(TAG, "Failed to create playback task!");
            vTaskDelete(pipeline->decode_task);
            pipeline->decode_task = NULL;
            pipeline->running = false;
            xSemaphoreGive(pipeline->mutex);
            return ESP_ERR_NO_MEM;
        }

        ring_buffer_set_consumer(pipeline->opus_buffer, pipeline->decode_task);
        ring_buffer_set_consumer(pipeline->pcm_buffer, pipeline->playback_task);

        ESP_LOGI(TAG, "RX pipeline started on core %d (event-driven)", AUDIO_CORE);
    }

    xSemaphoreGive(pipeline->mutex);
    return ESP_OK;
}

esp_err_t adf_pipeline_stop_impl(adf_pipeline_handle_t pipeline)
{
    if (!pipeline) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(pipeline->mutex, portMAX_DELAY);
    if (!pipeline->running) {
        xSemaphoreGive(pipeline->mutex);
        return ESP_OK;
    }
    pipeline->running = false;
    xSemaphoreGive(pipeline->mutex);

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

void adf_pipeline_destroy_impl(adf_pipeline_handle_t pipeline)
{
    if (!pipeline) {
        return;
    }

    adf_pipeline_stop_impl(pipeline);

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

bool adf_pipeline_is_running_impl(adf_pipeline_handle_t pipeline)
{
    if (!pipeline) {
        return false;
    }
    return pipeline->running;
}

esp_err_t adf_pipeline_get_stats_impl(adf_pipeline_handle_t pipeline, adf_pipeline_stats_t *stats)
{
    if (!pipeline || !stats) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(pipeline->mutex, portMAX_DELAY);
    memcpy(stats, &pipeline->stats, sizeof(adf_pipeline_stats_t));
    xSemaphoreGive(pipeline->mutex);

    return ESP_OK;
}

esp_err_t adf_pipeline_set_input_mode_impl(adf_pipeline_handle_t pipeline, adf_input_mode_t mode)
{
    if (!pipeline) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pipeline->type != ADF_PIPELINE_TX) {
        return ESP_ERR_INVALID_STATE;
    }

    pipeline->input_mode = mode;
    ESP_LOGI(TAG, "Input mode set to %d", mode);

    return ESP_OK;
}

adf_pipeline_handle_t adf_pipeline_get_latest_pipeline(void)
{
    return s_latest_pipeline;
}

TaskHandle_t adf_pipeline_get_capture_task(adf_pipeline_handle_t pipeline)
{
    return pipeline ? pipeline->capture_task : NULL;
}

TaskHandle_t adf_pipeline_get_encode_task(adf_pipeline_handle_t pipeline)
{
    return pipeline ? pipeline->encode_task : NULL;
}

TaskHandle_t adf_pipeline_get_decode_task(adf_pipeline_handle_t pipeline)
{
    return pipeline ? pipeline->decode_task : NULL;
}

TaskHandle_t adf_pipeline_get_playback_task(adf_pipeline_handle_t pipeline)
{
    return pipeline ? pipeline->playback_task : NULL;
}

// ---- Gain / Mute helpers ----

static float db_to_linear(float db, float min_db, float max_db)
{
    if (db <= min_db) return 0.0f;
    if (db > max_db) db = max_db;
    return powf(10.0f, db / 20.0f);
}

static float linear_to_db(float lin)
{
    if (lin <= 0.0f) return MIXER_MIN_GAIN_DB;
    float db = 20.0f * log10f(lin);
    if (db < MIXER_MIN_GAIN_DB) return MIXER_MIN_GAIN_DB;
    return db;
}

void adf_pipeline_set_output_gain_db(float db)
{
    adf_pipeline_handle_t p = s_latest_pipeline;
    if (!p) return;
    p->output_gain_linear = db_to_linear(db, MIXER_MIN_GAIN_DB, MIXER_MAX_OUTPUT_GAIN_DB);
}

float adf_pipeline_get_output_gain_db(void)
{
    adf_pipeline_handle_t p = s_latest_pipeline;
    if (!p) return linear_to_db(RX_OUTPUT_VOLUME);
    return linear_to_db(p->output_gain_linear);
}

void adf_pipeline_set_output_mute(bool mute)
{
    adf_pipeline_handle_t p = s_latest_pipeline;
    if (!p) return;
    p->output_mute = mute;
}

bool adf_pipeline_get_output_mute(void)
{
    adf_pipeline_handle_t p = s_latest_pipeline;
    if (!p) return false;
    return p->output_mute;
}

void adf_pipeline_set_input_gain_db(float db)
{
    adf_pipeline_handle_t p = s_latest_pipeline;
    if (!p) return;
    p->input_gain_linear = db_to_linear(db, MIXER_MIN_INPUT_GAIN_DB, MIXER_MAX_INPUT_GAIN_DB);
}

float adf_pipeline_get_input_gain_db(void)
{
    adf_pipeline_handle_t p = s_latest_pipeline;
    if (!p) return 0.0f;
    return linear_to_db(p->input_gain_linear);
}

void adf_pipeline_set_input_mute(bool mute)
{
    adf_pipeline_handle_t p = s_latest_pipeline;
    if (!p) return;
    p->input_mute = mute;
}

bool adf_pipeline_get_input_mute(void)
{
    adf_pipeline_handle_t p = s_latest_pipeline;
    if (!p) return false;
    return p->input_mute;
}

adf_input_mode_t adf_pipeline_get_input_mode(void)
{
    adf_pipeline_handle_t p = s_latest_pipeline;
    if (!p) return ADF_INPUT_MODE_AUX;
    return p->input_mode;
}

void adf_pipeline_set_input_mode_latest(adf_input_mode_t mode)
{
    adf_pipeline_handle_t p = s_latest_pipeline;
    if (!p) return;
    if (p->type != ADF_PIPELINE_TX) return;
    p->input_mode = mode;
}
