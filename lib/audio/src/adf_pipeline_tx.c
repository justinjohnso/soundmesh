#include "adf_pipeline_internal.h"

#include "audio/es8388_audio.h"
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

static uint16_t calculate_pcm_peak(const int16_t *samples, size_t sample_count)
{
    uint16_t peak = 0;
    if (!samples || sample_count == 0) {
        return peak;
    }

    for (size_t i = 0; i < sample_count; i++) {
        int32_t sample = samples[i];
        if (sample < 0) {
            sample = -sample;
        }
        if (sample > peak) {
            peak = (uint16_t)sample;
        }
    }
    return peak;
}

static void tx_update_input_activity(adf_pipeline_handle_t pipeline, bool signal_present, uint16_t peak)
{
    if (!pipeline) {
        return;
    }

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

void tx_capture_task(void *arg)
{
    adf_pipeline_handle_t pipeline = (adf_pipeline_handle_t)arg;
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
#define TX_TEST_TONE_MODE 0
#if TX_TEST_TONE_MODE
                static uint32_t tone_sample_offset = 0;
                const float freq = 440.0f;
                const float amplitude = 16000.0f;
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
                    ESP_LOGI(TAG, "Capture #%lu: stereo[0]=%d stereo[1]=%d mono[0]=%d mono[100]=%d stack_hwm=%u",
                             capture_count, (int)stereo_frame[0], (int)stereo_frame[1],
                             (int)mono_frame[0], (int)mono_frame[100],
                             (unsigned)uxTaskGetStackHighWaterMark(NULL));
                }

                if (pipeline->enable_local_output) {
                    esp_err_t lret = es8388_audio_write_stereo(stereo_frame, frames_read);
                    if (lret != ESP_OK) {
                        static uint32_t local_out_err_count = 0;
                        local_out_err_count++;
                        if ((local_out_err_count % 100) == 0) {
                            ESP_LOGW(TAG, "Local output write failed: %s (count=%lu)",
                                     esp_err_to_name(lret), local_out_err_count);
                        }
                    }
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
    while (1) {
        vTaskDelay(portMAX_DELAY);
    }
}

void tx_encode_task(void *arg)
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

        if (!pipeline->running) {
            break;
        }

        while (ring_buffer_available(pipeline->pcm_buffer) >= AUDIO_FRAME_BYTES) {
            esp_err_t ret = ring_buffer_read(pipeline->pcm_buffer, (uint8_t *)pcm_frame, AUDIO_FRAME_BYTES);
            if (ret != ESP_OK) {
                break;
            }

            bool signal_present = pipeline->stats.input_signal_present;
#if !SRC_CONTINUOUS_STREAMING
            if (pipeline->input_mode != ADF_INPUT_MODE_TONE && !signal_present) {
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

            uint8_t *dst = s_batch_buffer + batch_payload_len;
            dst[0] = (opus_len >> 8) & 0xFF;
            dst[1] = opus_len & 0xFF;
            memcpy(dst + 2, opus_frame, opus_len);
            batch_payload_len += 2 + opus_len;
            batch_count++;

            if (batch_count >= MESH_FRAMES_PER_PACKET) {
                ret = network_send_audio_batch(s_batch_buffer,
                                               batch_payload_len,
                                               pipeline->tx_seq,
                                               (uint32_t)(esp_timer_get_time() / 1000),
                                               batch_count,
                                               1);
                if (ret == ESP_OK) {
                    pipeline->stats.frames_processed += batch_count;
                } else if (ret != ESP_ERR_MESH_DISCONNECTED && ret != ESP_ERR_INVALID_STATE) {
                    pipeline->stats.frames_dropped += batch_count;
                }

                pipeline->tx_seq += batch_count;

                if ((pipeline->tx_seq & 0xFF) == 0) {
                    ESP_LOGI(TAG, "TX: seq=%u, batch=%u, payload=%u, enc=%luus, stack_hwm=%u",
                             pipeline->tx_seq, batch_count, (unsigned)batch_payload_len,
                             (unsigned long)pipeline->stats.avg_encode_time_us,
                             (unsigned)uxTaskGetStackHighWaterMark(NULL));
                }

                batch_count = 0;
                batch_payload_len = 0;
            }
        }
    }

    ESP_LOGI(TAG, "TX encode task exiting");
    while (1) {
        vTaskDelay(portMAX_DELAY);
    }
}
