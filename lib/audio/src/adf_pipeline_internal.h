#pragma once

#include "adf_pipeline_state.h"

esp_err_t adf_pipeline_create_impl(const adf_pipeline_config_t *config, adf_pipeline_handle_t *out_pipeline);
esp_err_t adf_pipeline_start_impl(adf_pipeline_handle_t pipeline);
esp_err_t adf_pipeline_stop_impl(adf_pipeline_handle_t pipeline);
void adf_pipeline_destroy_impl(adf_pipeline_handle_t pipeline);
bool adf_pipeline_is_running_impl(adf_pipeline_handle_t pipeline);

esp_err_t adf_pipeline_get_stats_impl(adf_pipeline_handle_t pipeline, adf_pipeline_stats_t *stats);
esp_err_t adf_pipeline_set_input_mode_impl(adf_pipeline_handle_t pipeline, adf_input_mode_t mode);

esp_err_t adf_pipeline_feed_opus_impl(adf_pipeline_handle_t pipeline,
                                      const uint8_t *opus_data,
                                      size_t opus_len,
                                      uint16_t seq,
                                      uint32_t timestamp);

esp_err_t adf_pipeline_get_fft_bins_impl(adf_pipeline_handle_t pipeline,
                                         float *bins_out,
                                         size_t bin_count,
                                         bool *valid_out);

void fft_process_frame(adf_pipeline_handle_t pipeline, const int16_t *samples, size_t sample_count);

void tx_capture_task(void *arg);
void tx_encode_task(void *arg);
void rx_decode_task(void *arg);
void rx_playback_task(void *arg);

adf_pipeline_handle_t adf_pipeline_get_latest_pipeline(void);
