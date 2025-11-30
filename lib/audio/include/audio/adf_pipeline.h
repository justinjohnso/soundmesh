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
    uint8_t buffer_fill_percent;
} adf_pipeline_stats_t;

esp_err_t adf_pipeline_get_stats(adf_pipeline_handle_t pipeline, adf_pipeline_stats_t *stats);

/**
 * TX input source modes
 */
typedef enum {
    ADF_INPUT_MODE_AUX,   // I2S/ES8388 line input (default)
    ADF_INPUT_MODE_TONE,  // Tone generator
    ADF_INPUT_MODE_USB    // USB audio (future)
} adf_input_mode_t;

/**
 * Set the TX pipeline input source mode
 * @param pipeline TX pipeline handle  
 * @param mode Input source mode
 * @return ESP_OK on success
 */
esp_err_t adf_pipeline_set_input_mode(adf_pipeline_handle_t pipeline, adf_input_mode_t mode);

#ifdef __cplusplus
}
#endif
