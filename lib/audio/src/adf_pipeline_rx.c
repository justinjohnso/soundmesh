#include "adf_pipeline_internal.h"

#include "audio/es8388_audio.h"
#include "audio/i2s_audio.h"
#include "audio/sequence_tracker.h"
#include "network/mesh_net.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <math.h>
#include <string.h>

static const char *TAG = "adf_pipeline";

#define RX_TEST_TONE_MODE 0

esp_err_t adf_pipeline_feed_opus_impl(adf_pipeline_handle_t pipeline,
                                      const uint8_t *opus_data,
                                      size_t opus_len,
                                      uint16_t seq,
                                      uint32_t timestamp)
{
    (void)timestamp;

    if (!pipeline || pipeline->type != ADF_PIPELINE_RX) {
        return ESP_ERR_INVALID_ARG;
    }

    if (opus_len > OPUS_MAX_FRAME_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }

    sequence_tracker_result_t seq_result =
        sequence_tracker_update(pipeline->first_rx_packet,
                                pipeline->last_rx_seq,
                                seq,
                                RX_PLC_MAX_FRAMES_PER_GAP);
    pipeline->first_rx_packet = seq_result.first_packet;
    pipeline->last_rx_seq = seq_result.last_seq;
    pipeline->stats.frames_dropped += seq_result.dropped_frames;

    if (seq_result.request_fec) {
        pipeline->pending_fec_recovery = true;
    } else if (seq_result.plc_frames_to_inject > 0) {
        pipeline->pending_fec_recovery = false;
        for (uint8_t i = 0; i < seq_result.plc_frames_to_inject; i++) {
            uint8_t plc_item[2] = {0, 0};
            if (ring_buffer_write(pipeline->opus_buffer, plc_item, sizeof(plc_item)) != ESP_OK) {
                break;
            }
        }
    }

    size_t needed = 2 + opus_len;
    size_t available_space = OPUS_BUFFER_SIZE - ring_buffer_available(pipeline->opus_buffer);
    if (available_space < needed) {
        pipeline->stats.frames_dropped++;
        return ESP_ERR_NO_MEM;
    }

    uint8_t tmp[2 + OPUS_MAX_FRAME_BYTES];
    tmp[0] = (opus_len >> 8) & 0xFF;
    tmp[1] = opus_len & 0xFF;
    memcpy(&tmp[2], opus_data, opus_len);

    esp_err_t ret = ring_buffer_write(pipeline->opus_buffer, tmp, 2 + opus_len);
    if (ret != ESP_OK) {
        pipeline->stats.frames_dropped++;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void rx_decode_task(void *arg)
{
    adf_pipeline_handle_t pipeline = (adf_pipeline_handle_t)arg;
    int16_t *pcm_frame = s_decode_pcm_frame;

    ESP_LOGI(TAG, "RX decode task started (event-driven, item-based)");

    while (pipeline->running) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));

        if (!pipeline->running) {
            break;
        }

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

            uint8_t first_bytes[4] = {0};
            if (opus_len >= 4) {
                first_bytes[0] = item[2];
                first_bytes[1] = item[3];
                first_bytes[2] = item[4];
                first_bytes[3] = item[5];
            }

            int samples_decoded = 0;
            if (opus_len == 0) {
                samples_decoded = opus_decode(
                    pipeline->decoder,
                    NULL,
                    0,
                    pcm_frame,
                    AUDIO_FRAME_SAMPLES,
                    0
                );
            } else {
                if (pipeline->pending_fec_recovery) {
                    int fec_samples = opus_decode(
                        pipeline->decoder,
                        &item[2],
                        opus_len,
                        pcm_frame,
                        AUDIO_FRAME_SAMPLES,
                        1
                    );
                    if (fec_samples > 0) {
                        fft_process_frame(pipeline, pcm_frame, (size_t)fec_samples);
                        size_t fec_bytes = fec_samples * sizeof(int16_t);
                        if (ring_buffer_write(pipeline->pcm_buffer, (uint8_t *)pcm_frame, fec_bytes) == ESP_OK) {
                            pipeline->stats.frames_processed++;
                        } else {
                            pipeline->stats.frames_dropped++;
                        }
                    }
                }
                samples_decoded = opus_decode(
                    pipeline->decoder,
                    &item[2],
                    opus_len,
                    pcm_frame,
                    AUDIO_FRAME_SAMPLES,
                    0
                );
                pipeline->pending_fec_recovery = false;
            }

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
    while (1) {
        vTaskDelay(portMAX_DELAY);
    }
}

void rx_playback_task(void *arg)
{
    adf_pipeline_handle_t pipeline = (adf_pipeline_handle_t)arg;
    int16_t *mono_frame = s_playback_mono_frame;
    int16_t *last_good_mono = s_playback_last_good_mono;
    int16_t *stereo_frame = s_playback_stereo_frame;
    int16_t *silence = s_playback_silence;

    bool prefilled = false;
    size_t prefill_bytes = AUDIO_FRAME_BYTES * JITTER_PREFILL_FRAMES;
    uint32_t consecutive_misses = 0;

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
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));

        if (!pipeline->running) {
            break;
        }

        size_t available = ring_buffer_available(pipeline->pcm_buffer);

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

        int frames_played = 0;

        available = ring_buffer_available(pipeline->pcm_buffer);

        if (available < AUDIO_FRAME_BYTES && prefilled) {
            vTaskDelay(pdMS_TO_TICKS(4));
            available = ring_buffer_available(pipeline->pcm_buffer);
        }

        if (available >= AUDIO_FRAME_BYTES) {
            esp_err_t ret = ring_buffer_read(pipeline->pcm_buffer, (uint8_t *)mono_frame, AUDIO_FRAME_BYTES);
            if (ret == ESP_OK) {
                static uint32_t playback_count = 0;
                playback_count++;

                if (pipeline->output_mute) {
                    memset(mono_frame, 0, AUDIO_FRAME_BYTES);
                } else if (fabsf(pipeline->output_gain_linear - 1.0f) > MIXER_GAIN_UNITY_EPSILON) {
                    float gain = pipeline->output_gain_linear;
                    for (size_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
                        int32_t scaled = (int32_t)(mono_frame[i] * gain);
                        if (scaled > 32767) scaled = 32767;
                        else if (scaled < -32768) scaled = -32768;
                        mono_frame[i] = (int16_t)scaled;
                    }
                }

                if (playback_count <= 5 || (playback_count % 500) == 0) {
                    ESP_LOGI(TAG, "Playback #%lu: s[0]=%d s[100]=%d",
                             playback_count, (int)mono_frame[0], (int)mono_frame[100]);
                }
                memcpy(last_good_mono, mono_frame, AUDIO_FRAME_BYTES);

#if defined(CONFIG_USE_ES8388)
                for (size_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
                    stereo_frame[2 * i]     = mono_frame[i];
                    stereo_frame[2 * i + 1] = mono_frame[i];
                }
                es8388_audio_write_stereo(stereo_frame, AUDIO_FRAME_SAMPLES);
#else
                i2s_audio_write_mono_as_stereo(mono_frame, AUDIO_FRAME_SAMPLES);
#endif
                frames_played = 1;
            }
        }

        if (frames_played == 0 && prefilled) {
            pipeline->stats.buffer_underruns++;
            consecutive_misses++;

            uint8_t dynamic_frames = network_get_jitter_prefill_frames();
            prefill_bytes = AUDIO_FRAME_BYTES * dynamic_frames;

            static uint32_t underrun_count = 0;
            underrun_count++;
            if (underrun_count <= 5 || (underrun_count % 20) == 0) {
                ESP_LOGW(TAG, "Underrun #%lu (miss=%lu) - prefill target: %d frames (%zu bytes)",
                         underrun_count, (unsigned long)consecutive_misses, dynamic_frames, prefill_bytes);
            }

#if defined(CONFIG_USE_ES8388)
            for (size_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
                stereo_frame[2 * i]     = last_good_mono[i];
                stereo_frame[2 * i + 1] = last_good_mono[i];
            }
            es8388_audio_write_stereo(stereo_frame, AUDIO_FRAME_SAMPLES);
#else
            i2s_audio_write_mono_as_stereo(last_good_mono, AUDIO_FRAME_SAMPLES);
#endif

            if (consecutive_misses >= 3) {
                prefilled = false;
                consecutive_misses = 0;
            }
        } else if (frames_played > 0) {
            consecutive_misses = 0;
        }

        pipeline->stats.buffer_fill_percent =
            (ring_buffer_available(pipeline->pcm_buffer) * 100) / PCM_BUFFER_SIZE;
    }

    ESP_LOGI(TAG, "RX playback task exiting");
    while (1) {
        vTaskDelay(portMAX_DELAY);
    }
}
