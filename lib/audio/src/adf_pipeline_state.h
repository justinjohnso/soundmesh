#pragma once

#include "audio/adf_pipeline.h"
#include "audio/ring_buffer.h"
#include "config/build.h"
#include "network/mesh_net.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "opus.h"

struct adf_pipeline {
    adf_pipeline_type_t type;
    volatile bool running;
    bool enable_local_output;
    volatile adf_input_mode_t input_mode;

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
    bool pending_fec_recovery;

    float fft_bins[FFT_PORTAL_BIN_COUNT];
    bool fft_valid;
    uint32_t fft_frame_counter;

    volatile float output_gain_linear;  // RX: applied per-sample in playback_task
    volatile bool  output_mute;         // RX: zero PCM frame before I2S write
    volatile float input_gain_linear;   // TX: applied per-sample after capture downmix
    volatile bool  input_mute;          // TX: silence captured PCM before encode
};

extern int16_t s_capture_stereo_frame[AUDIO_FRAME_SAMPLES * 2];
extern int16_t s_capture_mono_frame[AUDIO_FRAME_SAMPLES];
extern int16_t s_encode_pcm_frame[AUDIO_FRAME_SAMPLES];
extern uint8_t s_encode_opus_frame[OPUS_MAX_FRAME_BYTES];
extern int16_t s_decode_pcm_frame[AUDIO_FRAME_SAMPLES];
extern int16_t s_playback_mono_frame[AUDIO_FRAME_SAMPLES];
extern int16_t s_playback_last_good_mono[AUDIO_FRAME_SAMPLES];
extern int16_t s_playback_stereo_frame[AUDIO_FRAME_SAMPLES * 2];
extern int16_t s_playback_silence[AUDIO_FRAME_SAMPLES * 2];

extern uint8_t s_batch_buffer[NET_FRAME_HEADER_SIZE + MESH_FRAMES_PER_PACKET * (2 + OPUS_MAX_FRAME_BYTES)];
