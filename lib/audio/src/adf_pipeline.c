#include "audio/adf_pipeline.h"
#include "adf_pipeline_internal.h"

adf_pipeline_handle_t adf_pipeline_create(const adf_pipeline_config_t *config)
{
    adf_pipeline_handle_t pipeline = NULL;
    if (adf_pipeline_create_impl(config, &pipeline) != ESP_OK) {
        return NULL;
    }
    return pipeline;
}

esp_err_t adf_pipeline_start(adf_pipeline_handle_t pipeline)
{
    return adf_pipeline_start_impl(pipeline);
}

esp_err_t adf_pipeline_stop(adf_pipeline_handle_t pipeline)
{
    return adf_pipeline_stop_impl(pipeline);
}

void adf_pipeline_destroy(adf_pipeline_handle_t pipeline)
{
    adf_pipeline_destroy_impl(pipeline);
}

bool adf_pipeline_is_running(adf_pipeline_handle_t pipeline)
{
    return adf_pipeline_is_running_impl(pipeline);
}

esp_err_t adf_pipeline_feed_opus(adf_pipeline_handle_t pipeline,
                                 const uint8_t *opus_data,
                                 size_t opus_len,
                                 uint16_t seq,
                                 uint32_t timestamp)
{
    return adf_pipeline_feed_opus_impl(pipeline, opus_data, opus_len, seq, timestamp);
}

esp_err_t adf_pipeline_get_stats(adf_pipeline_handle_t pipeline, adf_pipeline_stats_t *stats)
{
    return adf_pipeline_get_stats_impl(pipeline, stats);
}

esp_err_t adf_pipeline_set_input_mode(adf_pipeline_handle_t pipeline, adf_input_mode_t mode)
{
    return adf_pipeline_set_input_mode_impl(pipeline, mode);
}

esp_err_t adf_pipeline_get_fft_bins(adf_pipeline_handle_t pipeline,
                                    float *bins_out,
                                    size_t bin_count,
                                    bool *valid_out)
{
    return adf_pipeline_get_fft_bins_impl(pipeline, bins_out, bin_count, valid_out);
}

esp_err_t adf_pipeline_get_latest_fft_bins(float *bins_out, size_t bin_count, bool *valid_out)
{
    adf_pipeline_handle_t pipeline = adf_pipeline_get_latest_pipeline();
    if (!pipeline) {
        if (valid_out) {
            *valid_out = false;
        }
        return ESP_ERR_NOT_FOUND;
    }
    return adf_pipeline_get_fft_bins_impl(pipeline, bins_out, bin_count, valid_out);
}
