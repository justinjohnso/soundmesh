#include "adf_pipeline_internal.h"

#include "audio/es8388_audio.h"
#include "audio/i2s_audio.h"
#include "audio/pcm_convert.h"
#include "audio/rx_underrun_concealment.h"
#include "audio/sequence_tracker.h"
#include "config/build.h"
#include "network/audio_transport.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <math.h>
#include <string.h>

static const char *TAG = "adf_pipeline";

void rx_decode_task(void *arg)
{
    adf_pipeline_handle_t pipeline = (adf_pipeline_handle_t)arg;
    int16_t *pcm_frame = s_decode_pcm_frame;
    uint16_t last_seq = 0;
    bool first_packet = true;

    ESP_LOGI(TAG, "RX decode task started (16-bit pure)");

    while (pipeline->running) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
        
        size_t item_size = 0;
        uint8_t *item = ring_buffer_receive_item(pipeline->opus_buffer, &item_size);
        while (item) {
            // Extract metadata from start of item
            // [flags:1][seq:2][ts:4][len:2][payload:N]
            uint16_t seq = (item[1] << 8) | item[2];
            uint16_t payload_len = (item[7] << 8) | item[8];
            uint8_t *payload = item + 9;

            if (first_packet) {
                last_seq = seq - MESH_FRAMES_PER_PACKET;
                first_packet = false;
            }

            // Handle Packet Loss Concealment (PLC)
            int gap = (uint16_t)(seq - last_seq) - MESH_FRAMES_PER_PACKET;
            if (gap > 0 && gap < 10) {
                for (int i = 0; i < gap; i++) {
                    opus_decode(pipeline->decoder, NULL, 0, pcm_frame, AUDIO_FRAME_SAMPLES, 1);
                    ring_buffer_write(pipeline->pcm_buffer, (uint8_t *)pcm_frame, AUDIO_FRAME_BYTES_MONO);
                }
            }

            int decoded = opus_decode(pipeline->decoder, payload, payload_len, pcm_frame, AUDIO_FRAME_SAMPLES, 0);
            if (decoded > 0) {
                ring_buffer_write(pipeline->pcm_buffer, (uint8_t *)pcm_frame, AUDIO_FRAME_BYTES_MONO);
                last_seq = seq;
            }

            ring_buffer_return_item(pipeline->opus_buffer, item);
            item = ring_buffer_receive_item(pipeline->opus_buffer, &item_size);
        }
    }
    vTaskDelete(NULL);
}

void rx_playback_task(void *arg)
{
    adf_pipeline_handle_t pipeline = (adf_pipeline_handle_t)arg;
    int16_t *mono_frame = s_playback_mono_frame;
    int16_t *stereo_frame = s_playback_stereo_frame;
    int16_t *last_good_mono = s_playback_last_good_mono;
    
    rx_underrun_state_t underrun_state;
    rx_underrun_reset(&underrun_state);

    ESP_LOGI(TAG, "RX playback task started (16-bit pure)");

    while (pipeline->running) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
        
        while (ring_buffer_available(pipeline->pcm_buffer) >= AUDIO_FRAME_BYTES_MONO) {
            if (ring_buffer_read(pipeline->pcm_buffer, (uint8_t *)mono_frame, AUDIO_FRAME_BYTES_MONO) != ESP_OK) break;

            float out_gain = RX_OUTPUT_VOLUME; // Simple baseline gain
            if (pipeline->output_mute) {
                memset(mono_frame, 0, AUDIO_FRAME_BYTES_MONO);
            } else if (fabsf(out_gain - 1.0f) > 0.001f) {
                for (size_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
                    mono_frame[i] = pcm_scale_s16(mono_frame[i], out_gain);
                }
            }

            memcpy(last_good_mono, mono_frame, AUDIO_FRAME_BYTES_MONO);
            pcm_convert_mono_to_stereo_s16(mono_frame, stereo_frame, AUDIO_FRAME_SAMPLES);
            es8388_audio_write_stereo(stereo_frame, AUDIO_FRAME_SAMPLES);
        }
    }
    vTaskDelete(NULL);
}

esp_err_t adf_pipeline_feed_opus_impl(adf_pipeline_handle_t p, const uint8_t *data, size_t len, uint16_t seq, uint32_t ts) {
    if (!p || !p->opus_buffer) return ESP_ERR_INVALID_ARG;
    
    // Minimal header for decode task: [flags:1][seq:2][ts:4][len:2] = 9 bytes
    uint8_t header[9];
    header[0] = 0; 
    header[1] = (seq >> 8) & 0xFF; header[2] = seq & 0xFF;
    header[3] = (ts >> 24) & 0xFF; header[4] = (ts >> 16) & 0xFF;
    header[5] = (ts >> 8) & 0xFF;  header[6] = ts & 0xFF;
    header[7] = (len >> 8) & 0xFF; header[8] = len & 0xFF;

    uint8_t *buf = malloc(len + 9);
    if (!buf) return ESP_ERR_NO_MEM;
    memcpy(buf, header, 9);
    memcpy(buf + 9, data, len);
    
    esp_err_t ret = ring_buffer_write(p->opus_buffer, buf, len + 9);
    free(buf);
    return ret;
}
