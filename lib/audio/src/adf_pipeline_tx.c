#include "adf_pipeline_internal.h"
#include "adf_pipeline_usb_fallback.h"

#include "audio/es8388_audio.h"
#include "audio/pcm_convert.h"
#include "audio/tone_gen.h"
#include "audio/usb_audio.h"
#include "network/audio_transport.h"

#include <esp_log.h>
#include <esp_mesh.h>
#include <esp_timer.h>
#include <math.h>
#include <string.h>

static const char *TAG = "adf_pipeline";
static uint8_t s_batch_buffer[MESH_OPUS_BATCH_MAX_BYTES];

#ifndef UNIT_TEST
static uint16_t calculate_pcm_peak_s16(const int16_t *samples, size_t sample_count)
{
    uint16_t peak = 0;
    if (!samples || sample_count == 0) return peak;
    for (size_t i = 0; i < sample_count; i++) {
        int16_t s = samples[i];
        uint16_t val = (s < 0) ? (uint16_t)(-s) : (uint16_t)s;
        if (val > peak) peak = val;
    }
    return peak;
}

static inline void apply_input_gain_s16_inplace(int16_t *samples, size_t sample_count, float gain_linear)
{
    if (!samples) return;
    for (size_t i = 0; i < sample_count; i++) {
        samples[i] = pcm_scale_s16(samples[i], gain_linear);
    }
}

static void tx_update_input_activity(adf_pipeline_handle_t pipeline, bool signal_present, uint16_t peak)
{
    if (!pipeline) return;
    pipeline->stats.input_peak = peak;
    if (signal_present) {
        pipeline->input_silence_frames = 0;
        pipeline->stats.input_signal_present = true;
    } else if (pipeline->input_silence_frames < AUDIO_INPUT_ACTIVITY_HOLD_FRAMES) {
        pipeline->input_silence_frames++;
    } else {
        pipeline->stats.input_signal_present = false;
    }
}
#endif

void tx_capture_task(void *arg)
{
    adf_pipeline_handle_t pipeline = (adf_pipeline_handle_t)arg;
    int16_t *stereo_frame = s_capture_stereo_frame;
    int16_t *mono_frame = s_capture_mono_frame;

    ESP_LOGI(TAG, "TX capture task started (16-bit pure)");

    static uint32_t no_data_count = 0;
    TickType_t last_wake_time = xTaskGetTickCount();
    const TickType_t frame_ticks = pdMS_TO_TICKS(AUDIO_FRAME_MS);
    adf_input_mode_t last_mode = ADF_INPUT_MODE_AUX;

    while (pipeline->running) {
        size_t frames_read = 0;
        esp_err_t ret = ESP_OK;
        adf_input_mode_t mode = pipeline->input_mode;
        
        if (mode != last_mode) {
            last_wake_time = xTaskGetTickCount();
            last_mode = mode;
        }

        switch (mode) {
            case ADF_INPUT_MODE_TONE:
                tone_gen_fill_buffer(mono_frame, AUDIO_FRAME_SAMPLES);
                frames_read = AUDIO_FRAME_SAMPLES;
                tx_update_input_activity(pipeline, true, 16000);
                if (pipeline->enable_local_output) {
                    pcm_convert_mono_to_stereo_s16(mono_frame, stereo_frame, frames_read);
                    es8388_audio_write_stereo(stereo_frame, frames_read);
                }
                vTaskDelayUntil(&last_wake_time, frame_ticks);
                break;

            case ADF_INPUT_MODE_USB:
                ret = usb_audio_read_stereo(stereo_frame, AUDIO_FRAME_SAMPLES, &frames_read);
                if (ret == ESP_OK && frames_read > 0) {
                    pcm_convert_stereo_to_mono_s16(stereo_frame, mono_frame, frames_read);
                    uint16_t peak = calculate_pcm_peak_s16(mono_frame, frames_read);
                    tx_update_input_activity(pipeline, peak >= AUDIO_INPUT_ACTIVITY_PEAK_THRESHOLD, peak);
                    if (pipeline->input_mute) memset(mono_frame, 0, frames_read * 2);
                    else if (fabsf(pipeline->input_gain_linear - 1.0f) > MIXER_GAIN_UNITY_EPSILON) {
                        apply_input_gain_s16_inplace(mono_frame, frames_read, pipeline->input_gain_linear);
                    }
                    if (pipeline->enable_local_output) es8388_audio_write_stereo(stereo_frame, frames_read);
                    last_wake_time = xTaskGetTickCount();
                } else {
                    vTaskDelayUntil(&last_wake_time, frame_ticks);
                }
                break;

            case ADF_INPUT_MODE_AUX:
            default:
                ret = es8388_audio_read_stereo(stereo_frame, AUDIO_FRAME_SAMPLES, &frames_read);
                if (ret != ESP_OK || frames_read == 0) {
                    tx_update_input_activity(pipeline, false, 0);
                    vTaskDelay(1);
                    last_wake_time = xTaskGetTickCount();
                    continue;
                }
                pcm_convert_stereo_to_mono_s16(stereo_frame, mono_frame, frames_read);
                uint16_t peak = calculate_pcm_peak_s16(mono_frame, frames_read);
                tx_update_input_activity(pipeline, peak >= AUDIO_INPUT_ACTIVITY_PEAK_THRESHOLD, peak);
                if (pipeline->input_mute) memset(mono_frame, 0, frames_read * 2);
                else if (fabsf(pipeline->input_gain_linear - 1.0f) > MIXER_GAIN_UNITY_EPSILON) {
                    apply_input_gain_s16_inplace(mono_frame, frames_read, pipeline->input_gain_linear);
                }
                if (pipeline->enable_local_output) es8388_audio_write_stereo(stereo_frame, frames_read);
                vTaskDelayUntil(&last_wake_time, frame_ticks);
                break;
        }

        if (frames_read > 0) {
            ring_buffer_write(pipeline->pcm_buffer, (const uint8_t *)mono_frame, frames_read * 2);
        }
    }
    vTaskDelete(NULL);
}

void tx_encode_task(void *arg)
{
    adf_pipeline_handle_t pipeline = (adf_pipeline_handle_t)arg;
    int16_t *pcm_frame = s_encode_pcm_frame;
    uint8_t *opus_frame = s_encode_opus_frame;
    uint32_t batch_count = 0;
    size_t batch_payload_len = 0;

    ESP_LOGI(TAG, "TX encode task started (16-bit, batch=%d)", MESH_FRAMES_PER_PACKET);

    while (pipeline->running) {
        ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(10));
        while (ring_buffer_available(pipeline->pcm_buffer) >= AUDIO_FRAME_BYTES_MONO) {
            if (ring_buffer_read(pipeline->pcm_buffer, (uint8_t *)pcm_frame, AUDIO_FRAME_BYTES_MONO) != ESP_OK) break;

#if !TX_CONTINUOUS_STREAMING
            if (pipeline->input_mode != ADF_INPUT_MODE_TONE && !pipeline->stats.input_signal_present) {
                batch_count = 0; batch_payload_len = 0; continue;
            }
#endif

            int64_t start_us = esp_timer_get_time();
            int opus_len = opus_encode(pipeline->encoder, pcm_frame, AUDIO_FRAME_SAMPLES, opus_frame, OPUS_MAX_FRAME_BYTES);
            uint32_t dur = (uint32_t)(esp_timer_get_time() - start_us);
            pipeline->stats.avg_encode_time_us = (pipeline->stats.avg_encode_time_us * 7 + dur) / 8;

            if (opus_len < 0) continue;

            uint8_t *dst = s_batch_buffer + batch_payload_len;
            dst[0] = (opus_len >> 8) & 0xFF;
            dst[1] = opus_len & 0xFF;
            memcpy(dst + 2, opus_frame, opus_len);
            batch_payload_len += 2 + opus_len;
            batch_count++;

            if (batch_count >= MESH_FRAMES_PER_PACKET) {
                network_send_audio_batch(s_batch_buffer, batch_payload_len, pipeline->tx_seq, 
                                         (uint32_t)(esp_timer_get_time()/1000), batch_count, 1);
                pipeline->stats.frames_processed += batch_count;
                pipeline->tx_seq += batch_count;
                batch_count = 0; batch_payload_len = 0;
            }
        }
    }
    vTaskDelete(NULL);
}
