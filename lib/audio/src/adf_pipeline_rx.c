#include "adf_pipeline_internal.h"

#include "audio/es8388_audio.h"
#include "audio/i2s_audio.h"
#include "audio/pcm_convert.h"
#include "audio/rx_underrun_concealment.h"
#include "audio/sequence_tracker.h"
#include "network/mesh_net.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <math.h>
#include <string.h>

static const char *TAG = "adf_pipeline";

#define RX_TEST_TONE_MODE 0

// Static buffers moved from stack to reduce task stack usage
static uint8_t s_feed_opus_tmp[3 + OPUS_MAX_FRAME_BYTES];  // flags + len + payload
static int32_t s_prefill_silence[AUDIO_FRAME_SAMPLES];

static volatile uint16_t s_runtime_out_gain_pct = OUT_OUTPUT_GAIN_DEFAULT_PCT;

#define OPUS_ITEM_FLAG_REQUEST_FEC 0x01

static inline void update_peak_u32(uint32_t *dst, uint32_t value)
{
    if (dst && value > *dst) {
        *dst = value;
    }
}

static inline int32_t scale_q15(int32_t sample, uint16_t gain_q15)
{
    int32_t scaled = ((int32_t)sample * (int32_t)gain_q15) / (int32_t)RX_UNDERRUN_GAIN_Q15_ONE;
    return pcm_clamp_s24_in_s32(scaled);
}

static inline void playback_wait_for_slot(int64_t *next_play_us, int64_t frame_period_us)
{
    if (!next_play_us || frame_period_us <= 0) {
        return;
    }

    int64_t now = esp_timer_get_time();
    if (*next_play_us == 0 || now > (*next_play_us + frame_period_us)) {
        *next_play_us = now;
    }

    if (now < *next_play_us) {
        int64_t wait_us = *next_play_us - now;
        if (wait_us >= 1000) {
            vTaskDelay(pdMS_TO_TICKS((wait_us + 999) / 1000));
        }
        now = esp_timer_get_time();
    }

    if (now > *next_play_us) {
        *next_play_us = now;
    }
    *next_play_us += frame_period_us;
}

static float current_out_gain_multiplier(void) {
    uint16_t gain = s_runtime_out_gain_pct;
    if (gain > OUT_OUTPUT_GAIN_MAX_PCT) {
        gain = OUT_OUTPUT_GAIN_MAX_PCT;
    }
    return (float)gain / 100.0f;
}

esp_err_t adf_pipeline_set_out_gain_percent(uint16_t out_gain_pct)
{
    if (out_gain_pct > OUT_OUTPUT_GAIN_MAX_PCT) {
        return ESP_ERR_INVALID_ARG;
    }
    s_runtime_out_gain_pct = out_gain_pct;
    return ESP_OK;
}

uint16_t adf_pipeline_get_out_gain_percent(void)
{
    return s_runtime_out_gain_pct;
}

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
                                RX_PLC_MAX_FRAMES_PER_GAP,
                                RX_MAX_STALE_FRAMES_TO_DROP);
    pipeline->first_rx_packet = seq_result.first_packet;
    pipeline->last_rx_seq = seq_result.last_seq;
    pipeline->stats.frames_dropped += seq_result.dropped_frames;
    pipeline->stats.rx_seq_gap_frames += seq_result.dropped_frames;
    if (seq_result.dropped_frames > 0) {
        pipeline->stats.rx_seq_gap_events++;
    }
    if (seq_result.request_fec) {
        pipeline->stats.rx_fec_requests++;
    }

    if (seq_result.hard_reset) {
        pipeline->stats.rx_hard_reset_events++;
        pipeline->stats.frames_dropped++;
        return ESP_OK;
    }

    if (seq_result.late_or_duplicate) {
        pipeline->stats.rx_late_or_duplicate_frames++;
        pipeline->stats.frames_dropped++;
        return ESP_OK;
    }

    if (seq_result.plc_frames_to_inject > 0) {
        pipeline->stats.rx_plc_events++;
        for (uint8_t i = 0; i < seq_result.plc_frames_to_inject; i++) {
            uint8_t plc_item[3] = {0, 0, 0};
            if (ring_buffer_write(pipeline->opus_buffer, plc_item, sizeof(plc_item)) != ESP_OK) {
                break;
            }
            pipeline->stats.rx_plc_frames_injected++;
        }
    }

    size_t needed = 3 + opus_len;
    size_t available_space = OPUS_BUFFER_SIZE - ring_buffer_available(pipeline->opus_buffer);
    if (available_space < needed) {
        pipeline->stats.rx_opus_buffer_overflows++;
        pipeline->stats.frames_dropped++;
        return ESP_ERR_NO_MEM;
    }

    // Use static buffer instead of stack allocation (was 515 bytes on stack)
    s_feed_opus_tmp[0] = seq_result.request_fec ? OPUS_ITEM_FLAG_REQUEST_FEC : 0;
    s_feed_opus_tmp[1] = (opus_len >> 8) & 0xFF;
    s_feed_opus_tmp[2] = opus_len & 0xFF;
    memcpy(&s_feed_opus_tmp[3], opus_data, opus_len);

    esp_err_t ret = ring_buffer_write(pipeline->opus_buffer, s_feed_opus_tmp, 3 + opus_len);
    if (ret != ESP_OK) {
        pipeline->stats.rx_opus_buffer_overflows++;
        pipeline->stats.frames_dropped++;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void rx_decode_task(void *arg)
{
    adf_pipeline_handle_t pipeline = (adf_pipeline_handle_t)arg;
    int16_t *pcm_frame = s_decode_pcm_frame;
    int32_t *decode_frame = s_decode_mono_frame;
    uint32_t decode_iteration = 0;

    ESP_LOGI(TAG, "RX decode task started (event-driven, item-based), stack_hwm=%u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    while (pipeline->running) {
        if (ring_buffer_available(pipeline->opus_buffer) == 0) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        }

        if (!pipeline->running) {
            break;
        }

        size_t item_size = 0;
        uint8_t *item;
        uint8_t items_processed = 0;

        while (items_processed < RX_DECODE_MAX_ITEMS_PER_CYCLE &&
               (item = ring_buffer_receive_item(pipeline->opus_buffer, &item_size)) != NULL) {
            if (ring_buffer_available(pipeline->pcm_buffer) >= RX_PCM_HIGH_WATER_BYTES) {
                ring_buffer_return_item(pipeline->opus_buffer, item);
                break;
            }

            if (item_size < 3) {
                ESP_LOGW(TAG, "Opus item too small: %u", (unsigned)item_size);
                ring_buffer_return_item(pipeline->opus_buffer, item);
                continue;
            }

            uint8_t item_flags = item[0];
            uint16_t opus_len = (item[1] << 8) | item[2];
            if (opus_len + 3 > item_size || opus_len > OPUS_MAX_FRAME_BYTES) {
                ESP_LOGW(TAG, "Invalid opus_len=%u for item_size=%u", opus_len, (unsigned)item_size);
                ring_buffer_return_item(pipeline->opus_buffer, item);
                continue;
            }

            int64_t start_us = esp_timer_get_time();

            uint8_t first_bytes[4] = {0};
            if (opus_len >= 4) {
                first_bytes[0] = item[3];
                first_bytes[1] = item[4];
                first_bytes[2] = item[5];
                first_bytes[3] = item[6];
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
                if ((item_flags & OPUS_ITEM_FLAG_REQUEST_FEC) != 0) {
                    int fec_samples = opus_decode(
                        pipeline->decoder,
                        &item[3],
                        opus_len,
                        pcm_frame,
                        AUDIO_FRAME_SAMPLES,
                        1
                    );
                    if (fec_samples > 0) {
                        fft_process_frame(pipeline, pcm_frame, (size_t)fec_samples);
                        pcm_convert_mono_s16_to_s24(pcm_frame, decode_frame, (size_t)fec_samples);
                        size_t fec_bytes = fec_samples * sizeof(int32_t);
                        if (ring_buffer_write(pipeline->pcm_buffer, (uint8_t *)decode_frame, fec_bytes) == ESP_OK) {
                            pipeline->stats.frames_processed++;
                        } else {
                            pipeline->stats.frames_dropped++;
                        }
                    }
                }
                samples_decoded = opus_decode(
                    pipeline->decoder,
                    &item[3],
                    opus_len,
                    pcm_frame,
                    AUDIO_FRAME_SAMPLES,
                    0
                );
            }

            ring_buffer_return_item(pipeline->opus_buffer, item);

            int64_t decode_time = esp_timer_get_time() - start_us;
            pipeline->stats.avg_decode_time_us =
                (pipeline->stats.avg_decode_time_us * 7 + (uint32_t)decode_time) / 8;

            if (samples_decoded < 0) {
                pipeline->stats.rx_decode_errors++;
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

            pcm_convert_mono_s16_to_s24(pcm_frame, decode_frame, (size_t)samples_decoded);
            size_t pcm_bytes = samples_decoded * sizeof(int32_t);
            esp_err_t ret = ring_buffer_write(pipeline->pcm_buffer, (uint8_t *)decode_frame, pcm_bytes);
            if (ret != ESP_OK) {
                pipeline->stats.frames_dropped++;
            } else {
                pipeline->stats.frames_processed++;
                decode_iteration++;
                // Log stack HWM every 1000 successful decodes
                if ((decode_iteration % 1000) == 0) {
                    ESP_LOGI(TAG, "Decode #%lu: avg_time=%luus, stack_hwm=%u",
                             (unsigned long)decode_iteration,
                             (unsigned long)pipeline->stats.avg_decode_time_us,
                             (unsigned)uxTaskGetStackHighWaterMark(NULL));
                }
            }

            items_processed++;
        }

        if (ring_buffer_available(pipeline->opus_buffer) > 0) {
            taskYIELD();
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
    int32_t *mono_frame = s_playback_mono_frame;
    int32_t *last_good_mono = s_playback_last_good_mono;
#if !defined(CONFIG_USE_ES8388)
    int16_t *mono_frame_s16 = s_playback_mono_frame_s16;
#endif
    int16_t *stereo_frame = s_playback_stereo_frame;
    int16_t *stereo_silence = s_playback_silence;

    bool prefilled = false;
    size_t prefill_bytes = AUDIO_FRAME_BYTES_INTERNAL_MONO * JITTER_PREFILL_FRAMES;
    uint8_t adaptive_prefill = JITTER_PREFILL_FRAMES;
    uint16_t rebuffer_prefill_frames = JITTER_PREFILL_FRAMES;
    uint8_t hysteresis_hold_remaining = 0;
    uint32_t consecutive_clean_playback_frames = 0;
    rx_underrun_state_t underrun_state = {0};
    const int64_t frame_period_us = (int64_t)AUDIO_FRAME_MS * 1000;
    int64_t next_play_us = 0;
    int64_t prefill_wait_start_ms = 0;
    int64_t last_rebuffer_ms = 0;

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
                next_play_us = 0;
                pipeline->stats.rx_prefill_events++;
                int64_t now_ms = esp_timer_get_time() / 1000;
                if (prefill_wait_start_ms > 0) {
                    if (now_ms > prefill_wait_start_ms) {
                        pipeline->stats.rx_prefill_wait_total_ms += (uint32_t)(now_ms - prefill_wait_start_ms);
                    }
                    prefill_wait_start_ms = 0;
                }
                static uint32_t prefill_count = 0;
                prefill_count++;
                hysteresis_hold_remaining = JITTER_HYSTERESIS_HOLD_FRAMES;
                if (prefill_count <= 3 || (prefill_count % 50) == 0) {
                    int64_t since_last_rebuffer_ms = 0;
                    if (last_rebuffer_ms > 0 && now_ms > last_rebuffer_ms) {
                        since_last_rebuffer_ms = now_ms - last_rebuffer_ms;
                    }
                    ESP_LOGI(TAG, "Playback prefilled #%lu (%zu bytes, since_rebuffer=%lldms)",
                             prefill_count, available, (long long)since_last_rebuffer_ms);
                }
            } else {
                if (prefill_wait_start_ms == 0) {
                    prefill_wait_start_ms = esp_timer_get_time() / 1000;
                }
#if defined(CONFIG_USE_ES8388)
                es8388_audio_write_stereo(stereo_silence, AUDIO_FRAME_SAMPLES);
#else
                pcm_convert_mono_s24_to_s16(s_prefill_silence, mono_frame_s16, AUDIO_FRAME_SAMPLES);
                i2s_audio_write_mono_as_stereo(mono_frame_s16, AUDIO_FRAME_SAMPLES);
#endif
                continue;
            }
        }

        int frames_played = 0;
        bool played_audio_frame = false;

        available = ring_buffer_available(pipeline->pcm_buffer);

        if (prefilled && hysteresis_hold_remaining > 0) {
            hysteresis_hold_remaining--;
#if defined(CONFIG_USE_ES8388)
            playback_wait_for_slot(&next_play_us, frame_period_us);
            es8388_audio_write_stereo(stereo_silence, AUDIO_FRAME_SAMPLES);
#else
            playback_wait_for_slot(&next_play_us, frame_period_us);
            pcm_convert_mono_s24_to_s16(s_prefill_silence, mono_frame_s16, AUDIO_FRAME_SAMPLES);
            i2s_audio_write_mono_as_stereo(mono_frame_s16, AUDIO_FRAME_SAMPLES);
#endif
            frames_played = 1;
        }

        if (frames_played == 0 && available < AUDIO_FRAME_BYTES_INTERNAL_MONO && prefilled) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(4));
            available = ring_buffer_available(pipeline->pcm_buffer);
        }

        if (frames_played == 0 && available >= AUDIO_FRAME_BYTES_INTERNAL_MONO) {
            esp_err_t ret = ring_buffer_read(pipeline->pcm_buffer, (uint8_t *)mono_frame, AUDIO_FRAME_BYTES_INTERNAL_MONO);
            if (ret == ESP_OK) {
                static uint32_t playback_count = 0;
                playback_count++;
                float out_gain = current_out_gain_multiplier();

                if (pipeline->output_mute) {
                    memset(mono_frame, 0, AUDIO_FRAME_BYTES_INTERNAL_MONO);
                } else if (fabsf(out_gain - 1.0f) > MIXER_GAIN_UNITY_EPSILON) {
                    for (size_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
                        int32_t scaled = (int32_t)(mono_frame[i] * out_gain);
                        mono_frame[i] = pcm_clamp_s24_in_s32(scaled);
                    }
                }

                if (playback_count <= 5 || (playback_count % 1000) == 0) {
                    ESP_LOGI(TAG, "Playback #%lu: s[0]=%d s[100]=%d stack_hwm=%u",
                             playback_count,
                             (int)pcm_s24_in_s32_to_s16(mono_frame[0]),
                             (int)pcm_s24_in_s32_to_s16(mono_frame[100]),
                             (unsigned)uxTaskGetStackHighWaterMark(NULL));
                }
                memcpy(last_good_mono, mono_frame, AUDIO_FRAME_BYTES_INTERNAL_MONO);

#if defined(CONFIG_USE_ES8388)
                playback_wait_for_slot(&next_play_us, frame_period_us);
                pcm_convert_mono_s24_to_stereo_s16(mono_frame, stereo_frame, AUDIO_FRAME_SAMPLES);
                es8388_audio_write_stereo(stereo_frame, AUDIO_FRAME_SAMPLES);
#else
                playback_wait_for_slot(&next_play_us, frame_period_us);
                pcm_convert_mono_s24_to_s16(mono_frame, mono_frame_s16, AUDIO_FRAME_SAMPLES);
                i2s_audio_write_mono_as_stereo(mono_frame_s16, AUDIO_FRAME_SAMPLES);
#endif
                frames_played = 1;
                played_audio_frame = true;
            }
        }

        if (frames_played == 0 && prefilled) {
            consecutive_clean_playback_frames = 0;
            pipeline->stats.buffer_underruns++;
            rx_underrun_action_t underrun_action = rx_underrun_on_miss(&underrun_state);
            update_peak_u32(&pipeline->stats.rx_consecutive_miss_peak, underrun_action.consecutive_misses);

            static uint32_t underrun_count = 0;
            underrun_count++;
            if (underrun_count <= 5 || (underrun_count % 20) == 0) {
                ESP_LOGW(TAG,
                         "Underrun #%lu (consecutive_misses=%lu adaptive_prefill=%u) - prefill target: %u frames (%zu bytes)",
                         underrun_count, (unsigned long)underrun_action.consecutive_misses,
                         adaptive_prefill, rebuffer_prefill_frames, prefill_bytes);
            }

            int32_t *conceal_frame = last_good_mono;
            if (pipeline->output_mute || underrun_action.gain_q15 == 0) {
                conceal_frame = s_prefill_silence;
            } else if (underrun_action.gain_q15 < RX_UNDERRUN_GAIN_Q15_ONE) {
                for (size_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
                    mono_frame[i] = scale_q15(last_good_mono[i], underrun_action.gain_q15);
                }
                conceal_frame = mono_frame;
            }

#if defined(CONFIG_USE_ES8388)
            playback_wait_for_slot(&next_play_us, frame_period_us);
            pcm_convert_mono_s24_to_stereo_s16(conceal_frame, stereo_frame, AUDIO_FRAME_SAMPLES);
            es8388_audio_write_stereo(stereo_frame, AUDIO_FRAME_SAMPLES);
#else
            playback_wait_for_slot(&next_play_us, frame_period_us);
            pcm_convert_mono_s24_to_s16(conceal_frame, mono_frame_s16, AUDIO_FRAME_SAMPLES);
            i2s_audio_write_mono_as_stereo(mono_frame_s16, AUDIO_FRAME_SAMPLES);
#endif

            if (underrun_action.force_rebuffer) {
                prefilled = false;
                prefill_wait_start_ms = esp_timer_get_time() / 1000;
                last_rebuffer_ms = prefill_wait_start_ms;
                next_play_us = 0;
                hysteresis_hold_remaining = 0;
                pipeline->stats.rx_underrun_rebuffer_events++;
                if (adaptive_prefill < JITTER_BUFFER_FRAMES) {
                    adaptive_prefill++;
                }
                uint16_t network_prefill_frames = network_get_jitter_prefill_frames();
                if (network_prefill_frames < JITTER_PREFILL_FRAMES) {
                    network_prefill_frames = JITTER_PREFILL_FRAMES;
                } else if (network_prefill_frames > JITTER_BUFFER_FRAMES) {
                    network_prefill_frames = JITTER_BUFFER_FRAMES;
                }
                rebuffer_prefill_frames = adaptive_prefill;
                if (rebuffer_prefill_frames < network_prefill_frames) {
                    rebuffer_prefill_frames = network_prefill_frames;
                }
                prefill_bytes = AUDIO_FRAME_BYTES_INTERNAL_MONO * rebuffer_prefill_frames;
                rx_underrun_reset(&underrun_state);
            }
        } else if (frames_played > 0) {
            rx_underrun_reset(&underrun_state);
            if (played_audio_frame) {
                if (consecutive_clean_playback_frames < UINT32_MAX) {
                    consecutive_clean_playback_frames++;
                }
                if (consecutive_clean_playback_frames >= JITTER_ADAPTIVE_DECAY_FRAMES) {
                    if (adaptive_prefill > JITTER_PREFILL_FRAMES) {
                        adaptive_prefill--;
                    }
                    consecutive_clean_playback_frames = 0;
                }
            }
        }

        pipeline->stats.buffer_fill_percent =
            (ring_buffer_available(pipeline->pcm_buffer) * 100) / PCM_BUFFER_SIZE;
        if (pipeline->stats.buffer_fill_percent > pipeline->stats.buffer_fill_peak_percent) {
            pipeline->stats.buffer_fill_peak_percent = pipeline->stats.buffer_fill_percent;
        }
    }

    ESP_LOGI(TAG, "RX playback task exiting");
    while (1) {
        vTaskDelay(portMAX_DELAY);
    }
}
