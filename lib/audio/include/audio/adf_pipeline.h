#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ESP-ADF-style Audio Pipeline for MeshNet Audio
 * 
 * This module provides task-based audio pipelines using esp_audio_codec
 * for Opus encoding/decoding with ringbuffer-based data flow.
 * 
 * TX Pipeline: i2s_reader → opus_encoder → mesh_output
 * RX Pipeline: mesh_input → opus_decoder → i2s_writer
 */

// Pipeline handle (opaque)
typedef struct adf_pipeline* adf_pipeline_handle_t;

// Pipeline type
typedef enum {
    ADF_PIPELINE_TX,    // Capture → Encode → Mesh send
    ADF_PIPELINE_RX     // Mesh receive → Decode → Playback
} adf_pipeline_type_t;

// Pipeline configuration
typedef struct {
    adf_pipeline_type_t type;
    bool enable_local_output;   // For COMBO: enable DAC output alongside mesh TX
    uint32_t opus_bitrate;      // Target bitrate (default 64000)
    uint8_t opus_complexity;    // Encoder complexity 0-10 (default 5)
} adf_pipeline_config_t;

// Default configuration
#define ADF_PIPELINE_CONFIG_DEFAULT() { \
    .type = ADF_PIPELINE_TX, \
    .enable_local_output = false, \
    .opus_bitrate = 64000, \
    .opus_complexity = 5 \
}

/**
 * Create and initialize an audio pipeline
 * @param config Pipeline configuration
 * @return Pipeline handle or NULL on failure
 */
adf_pipeline_handle_t adf_pipeline_create(const adf_pipeline_config_t *config);

/**
 * Start the pipeline (begins audio processing)
 * @param pipeline Pipeline handle
 * @return ESP_OK on success
 */
esp_err_t adf_pipeline_start(adf_pipeline_handle_t pipeline);

/**
 * Stop the pipeline
 * @param pipeline Pipeline handle
 * @return ESP_OK on success
 */
esp_err_t adf_pipeline_stop(adf_pipeline_handle_t pipeline);

/**
 * Destroy the pipeline and free resources
 * @param pipeline Pipeline handle
 */
void adf_pipeline_destroy(adf_pipeline_handle_t pipeline);

/**
 * Check if pipeline is running
 * @param pipeline Pipeline handle
 * @return true if running
 */
bool adf_pipeline_is_running(adf_pipeline_handle_t pipeline);

/**
 * Feed Opus data to RX pipeline (called from mesh RX callback)
 * @param pipeline RX pipeline handle
 * @param opus_data Opus frame data
 * @param opus_len Opus frame length
 * @param seq Sequence number for ordering/PLC
 * @param timestamp Sender timestamp
 * @return ESP_OK on success, ESP_ERR_NO_MEM if buffer full
 */
esp_err_t adf_pipeline_feed_opus(adf_pipeline_handle_t pipeline,
                                  const uint8_t *opus_data, size_t opus_len,
                                  uint16_t seq, uint32_t timestamp);

/**
 * Get pipeline statistics
 */
typedef struct {
    uint32_t frames_processed;
    uint32_t frames_dropped;
    uint32_t buffer_underruns;
    uint32_t avg_encode_time_us;
    uint32_t avg_decode_time_us;
    uint32_t rx_seq_gap_events;
    uint32_t rx_seq_gap_frames;
    uint32_t rx_late_or_duplicate_frames;
    uint32_t rx_hard_reset_events;
    uint32_t rx_fec_requests;
    uint32_t rx_plc_events;
    uint32_t rx_plc_frames_injected;
    uint32_t rx_opus_buffer_overflows;
    uint32_t rx_decode_errors;
    uint32_t rx_underrun_rebuffer_events;
    uint32_t rx_prefill_events;
    uint32_t rx_prefill_wait_total_ms;
    uint32_t rx_consecutive_miss_peak;
    uint8_t buffer_fill_percent;
    uint8_t buffer_fill_peak_percent;
    uint16_t input_peak;
    bool input_signal_present;
    bool usb_input_ready;
    bool usb_input_active;
    bool usb_fallback_to_aux;
} adf_pipeline_stats_t;

esp_err_t adf_pipeline_get_stats(adf_pipeline_handle_t pipeline, adf_pipeline_stats_t *stats);

/**
 * TX input source modes
 */
typedef enum {
    ADF_INPUT_MODE_AUX,   // I2S/ES8388 line input (default)
    ADF_INPUT_MODE_TONE,  // Tone generator
    ADF_INPUT_MODE_USB    // USB audio
} adf_input_mode_t;

/**
 * Set the TX pipeline input source mode
 * @param pipeline TX pipeline handle  
 * @param mode Input source mode
 * @return ESP_OK on success
 */
esp_err_t adf_pipeline_set_input_mode(adf_pipeline_handle_t pipeline, adf_input_mode_t mode);

/**
 * Runtime OUT playback gain control (software mixer).
 * Value is percentage where 100 = unity gain.
 */
esp_err_t adf_pipeline_set_out_gain_percent(uint16_t out_gain_pct);
uint16_t adf_pipeline_get_out_gain_percent(void);

/**
 * Get latest normalized FFT bins for portal visualization.
 * @param pipeline Pipeline handle (TX or RX)
 * @param bins_out Output array
 * @param bin_count Number of bins to copy
 * @param valid_out Optional validity flag (true once at least one FFT frame is computed)
 * @return ESP_OK on success
 */
esp_err_t adf_pipeline_get_fft_bins(adf_pipeline_handle_t pipeline,
                                    float *bins_out,
                                    size_t bin_count,
                                    bool *valid_out);

/**
 * Get latest FFT bins from the active local pipeline.
 * Useful for portal telemetry without direct access to pipeline handle.
 */
esp_err_t adf_pipeline_get_latest_fft_bins(float *bins_out, size_t bin_count, bool *valid_out);

/**
 * Output gain and mute controls (primarily for RX/COMBO playback).
 * Changes take effect on the next audio frame; no restart required.
 * db clamped to [-60.0, +12.0]. Values <= -60 dB are treated as –inf (silence).
 */
void  adf_pipeline_set_output_gain_db(float db);
float adf_pipeline_get_output_gain_db(void);
void  adf_pipeline_set_output_mute(bool mute);
bool  adf_pipeline_get_output_mute(void);

/**
 * Set the node's positional coordinates for DSP effects (e.g. LPF).
 */
void  adf_pipeline_set_position(float x, float y, float z);

/**
 * Input gain and mute controls (TX capture trim, applied before Opus encode).
 * db clamped to [-18.0, +18.0].
 */
void  adf_pipeline_set_input_gain_db(float db);
float adf_pipeline_get_input_gain_db(void);
void  adf_pipeline_set_input_mute(bool mute);
bool  adf_pipeline_get_input_mute(void);

/**
 * Query the current input mode of the active pipeline (for portal serialization).
 * Returns ADF_INPUT_MODE_AUX when no pipeline is active.
 */
adf_input_mode_t adf_pipeline_get_input_mode(void);

/**
 * Set the input mode on the active (latest) pipeline without needing the handle.
 * No-op when no pipeline is active.
 */
void adf_pipeline_set_input_mode_latest(adf_input_mode_t mode);

#ifdef __cplusplus
}
#endif
