#include "adf_pipeline_internal.h"

#include "audio/es8388_audio.h"
#include "audio/i2s_audio.h"
#include "audio/usb_audio.h"
#include "config/build_role.h"
#include "network/mesh_net.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <math.h>
#include <string.h>

static const char *TAG = "adf_pipeline";

// Pure 16-bit internal buffers
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
    pipeline->encoder = opus_encoder_create(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS_MONO, OPUS_APPLICATION_AUDIO, &error);
    if (error != OPUS_OK || !pipeline->encoder) return ESP_FAIL;

    opus_encoder_ctl(pipeline->encoder, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(pipeline->encoder, OPUS_SET_COMPLEXITY(complexity));
    opus_encoder_ctl(pipeline->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    opus_encoder_ctl(pipeline->encoder, OPUS_SET_VBR(1));
    opus_encoder_ctl(pipeline->encoder, OPUS_SET_INBAND_FEC(OPUS_ENABLE_INBAND_FEC));
    opus_encoder_ctl(pipeline->encoder, OPUS_SET_PACKET_LOSS_PERC(OPUS_EXPECTED_LOSS_PCT));

    ESP_LOGI(TAG, "Opus 16-bit encoder initialized: %lubps, complexity=%d", (unsigned long)bitrate, complexity);
    return ESP_OK;
}

static esp_err_t init_opus_decoder(adf_pipeline_handle_t pipeline)
{
    int error;
    pipeline->decoder = opus_decoder_create(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS_MONO, &error);
    if (error != OPUS_OK || !pipeline->decoder) return ESP_FAIL;
    opus_decoder_ctl(pipeline->decoder, OPUS_SET_GAIN(0));
    ESP_LOGI(TAG, "Opus 16-bit decoder initialized");
    return ESP_OK;
}

esp_err_t adf_pipeline_create_impl(const adf_pipeline_config_t *config, adf_pipeline_handle_t *out_pipeline)
{
    struct adf_pipeline *pipeline = calloc(1, sizeof(struct adf_pipeline));
    if (!pipeline) return ESP_ERR_NO_MEM;

    pipeline->type = config->type;
    pipeline->enable_local_output = config->enable_local_output;
    pipeline->input_mode = ADF_INPUT_MODE_AUX;
    pipeline->input_gain_linear = 1.0f;
    pipeline->output_gain_linear = 1.0f;
    pipeline->x = 0.0f;
    pipeline->y = 0.0f;
    pipeline->z = 0.0f;
    pipeline->mutex = xSemaphoreCreateMutex();
    
    // Allocate buffers in internal SRAM for speed and reliability (since they are now smaller)
    pipeline->pcm_buffer = ring_buffer_create_ex(PCM_BUFFER_SIZE, false, false);
    pipeline->opus_buffer = ring_buffer_create_ex(OPUS_BUFFER_SIZE, true, false);

    if (pipeline->type == ADF_PIPELINE_TX) {
        init_opus_encoder(pipeline, OPUS_BITRATE, OPUS_COMPLEXITY);
    } else {
        init_opus_decoder(pipeline);
    }

    *out_pipeline = pipeline;
    s_latest_pipeline = pipeline;
    return ESP_OK;
}

esp_err_t adf_pipeline_start_impl(adf_pipeline_handle_t p) {
    p->running = true;
    if (p->type == ADF_PIPELINE_TX) {
        xTaskCreatePinnedToCore(tx_capture_task, "adf_cap", CAPTURE_TASK_STACK, p, CAPTURE_TASK_PRIO, &p->capture_task, 1);
        xTaskCreatePinnedToCore(tx_encode_task, "adf_enc", ENCODE_TASK_STACK, p, ENCODE_TASK_PRIO, &p->encode_task, 1);
        ring_buffer_set_consumer(p->pcm_buffer, p->encode_task);
    } else {
        xTaskCreatePinnedToCore(rx_decode_task, "adf_dec", DECODE_TASK_STACK, p, DECODE_TASK_PRIO, &p->decode_task, 1);
        xTaskCreatePinnedToCore(rx_playback_task, "adf_play", PLAYBACK_TASK_STACK, p, PLAYBACK_TASK_PRIO, &p->playback_task, 1);
        ring_buffer_set_consumer(p->opus_buffer, p->decode_task);
        ring_buffer_set_consumer(p->pcm_buffer, p->playback_task);
    }
    return ESP_OK;
}

esp_err_t adf_pipeline_stop_impl(adf_pipeline_handle_t p) {
    p->running = false;
    return ESP_OK;
}

void adf_pipeline_destroy_impl(adf_pipeline_handle_t p) {
    if (p->encoder) opus_encoder_destroy(p->encoder);
    if (p->decoder) opus_decoder_destroy(p->decoder);
    ring_buffer_destroy(p->pcm_buffer);
    ring_buffer_destroy(p->opus_buffer);
    vSemaphoreDelete(p->mutex);
    free(p);
}

bool adf_pipeline_is_running_impl(adf_pipeline_handle_t p) { return p->running; }
esp_err_t adf_pipeline_get_stats_impl(adf_pipeline_handle_t p, adf_pipeline_stats_t *s) {
    memcpy(s, &p->stats, sizeof(adf_pipeline_stats_t));
    return ESP_OK;
}
esp_err_t adf_pipeline_set_input_mode_impl(adf_pipeline_handle_t p, adf_input_mode_t m) {
    p->input_mode = m;
    return ESP_OK;
}

adf_input_mode_t adf_pipeline_get_input_mode(adf_pipeline_handle_t p) {
    return p ? p->input_mode : ADF_INPUT_MODE_AUX;
}

TaskHandle_t adf_pipeline_get_capture_task(adf_pipeline_handle_t p) {
    return p ? p->capture_task : NULL;
}

TaskHandle_t adf_pipeline_get_encode_task(adf_pipeline_handle_t p) {
    return p ? p->encode_task : NULL;
}

void adf_pipeline_set_output_gain_db(adf_pipeline_handle_t p, float db) {
    if (!p) return;
    if (db <= -60.0f) p->output_gain_linear = 0.0f;
    else p->output_gain_linear = powf(10.0f, db / 20.0f);
}

float adf_pipeline_get_output_gain_db(adf_pipeline_handle_t p) {
    if (!p || p->output_gain_linear <= 0.0f) return -60.0f;
    return 20.0f * log10f(p->output_gain_linear);
}

void adf_pipeline_set_output_mute(adf_pipeline_handle_t p, bool mute) {
    if (p) p->output_mute = mute;
}

bool adf_pipeline_get_output_mute(adf_pipeline_handle_t p) {
    return p ? p->output_mute : false;
}

void adf_pipeline_set_input_gain_db(adf_pipeline_handle_t p, float db) {
    if (!p) return;
    p->input_gain_linear = powf(10.0f, db / 20.0f);
}

float adf_pipeline_get_input_gain_db(adf_pipeline_handle_t p) {
    if (!p || p->input_gain_linear <= 0.0f) return -60.0f;
    return 20.0f * log10f(p->input_gain_linear);
}

void adf_pipeline_set_input_mute(adf_pipeline_handle_t p, bool mute) {
    if (p) p->input_mute = mute;
}

bool adf_pipeline_get_input_mute(adf_pipeline_handle_t p) {
    return p ? p->input_mute : false;
}

void adf_pipeline_set_position(adf_pipeline_handle_t p, float x, float y, float z) {
    if (!p) return;
    p->x = x; p->y = y; p->z = z;
}

adf_pipeline_handle_t adf_pipeline_get_latest_pipeline(void) { return s_latest_pipeline; }
void adf_pipeline_set_input_mode_latest(adf_input_mode_t m) { adf_pipeline_set_input_mode_impl(s_latest_pipeline, m); }
