#include "adf_pipeline_internal.h"

#include <esp_dsp.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include "dsps_fft2r.h"
#include "dsps_wind_hann.h"

#include <math.h>
#include <string.h>

static const char *TAG = "adf_pipeline";

static float s_fft_window[FFT_ANALYSIS_SIZE];
static float s_fft_complex[FFT_ANALYSIS_SIZE * 2];
static bool s_fft_initialized = false;
static bool s_fft_init_attempted = false;
static int s_fft_bar_start[FFT_PORTAL_BIN_COUNT];
static int s_fft_bar_end[FFT_PORTAL_BIN_COUNT];

static void fft_init_once(void)
{
    if (s_fft_init_attempted) {
        return;
    }
    s_fft_init_attempted = true;

#ifdef CONFIG_COMBO_BUILD
    ESP_LOGW(TAG, "FFT disabled on COMBO build");
    return;
#endif

    ESP_LOGI(TAG, "FFT init: calling dsps_fft2r_init_fc32 (heap=%lu)...",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));

    esp_err_t ret = dsps_fft2r_init_fc32(NULL, CONFIG_DSP_MAX_FFT_SIZE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp-dsp FFT init failed: %d", ret);
        return;
    }

    dsps_wind_hann_f32(s_fft_window, FFT_ANALYSIS_SIZE);

    const float ratio = (float)FFT_MAX_FREQ_HZ / (float)FFT_MIN_FREQ_HZ;
    const int max_bin = (FFT_ANALYSIS_SIZE / 2) - 1;

    for (int i = 0; i < FFT_PORTAL_BIN_COUNT; i++) {
        float f0 = (float)FFT_MIN_FREQ_HZ * powf(ratio, (float)i / (float)FFT_PORTAL_BIN_COUNT);
        float f1 = (float)FFT_MIN_FREQ_HZ * powf(ratio, (float)(i + 1) / (float)FFT_PORTAL_BIN_COUNT);

        int k0 = (int)floorf((f0 * (float)FFT_ANALYSIS_SIZE) / (float)AUDIO_SAMPLE_RATE);
        int k1 = (int)ceilf((f1 * (float)FFT_ANALYSIS_SIZE) / (float)AUDIO_SAMPLE_RATE);

        if (k0 < 1) k0 = 1;
        if (k0 > max_bin) k0 = max_bin;
        if (k1 <= k0) k1 = k0 + 1;
        if (k1 > max_bin + 1) k1 = max_bin + 1;

        s_fft_bar_start[i] = k0;
        s_fft_bar_end[i] = k1;
    }

    s_fft_initialized = true;
    ESP_LOGI(TAG, "FFT init complete: size=%d, bars=%d", FFT_ANALYSIS_SIZE, FFT_PORTAL_BIN_COUNT);
}

void fft_process_frame(adf_pipeline_handle_t pipeline, const int16_t *samples, size_t sample_count)
{
    if (!pipeline || !samples || sample_count < FFT_ANALYSIS_SIZE) {
        return;
    }

    pipeline->fft_frame_counter++;
    if ((pipeline->fft_frame_counter % FFT_UPDATE_INTERVAL_FRAMES) != 0) {
        return;
    }

    if (!s_fft_initialized) {
        fft_init_once();
        if (!s_fft_initialized) {
            return;
        }
    }

    const size_t offset = sample_count - FFT_ANALYSIS_SIZE;
    for (int i = 0; i < FFT_ANALYSIS_SIZE; i++) {
        float normalized = (float)samples[offset + i] / 32768.0f;
        s_fft_complex[i * 2 + 0] = normalized * s_fft_window[i];
        s_fft_complex[i * 2 + 1] = 0.0f;
    }

    esp_err_t ret = dsps_fft2r_fc32(s_fft_complex, FFT_ANALYSIS_SIZE);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "FFT compute failed: %d", ret);
        return;
    }

    dsps_bit_rev_fc32(s_fft_complex, FFT_ANALYSIS_SIZE);

    float bins_local[FFT_PORTAL_BIN_COUNT];
    const float db_span = (FFT_DB_CEIL - FFT_DB_FLOOR) > 0.01f ? (FFT_DB_CEIL - FFT_DB_FLOOR) : 1.0f;

    for (int b = 0; b < FFT_PORTAL_BIN_COUNT; b++) {
        float peak_db = FFT_DB_FLOOR;
        for (int k = s_fft_bar_start[b]; k < s_fft_bar_end[b]; k++) {
            float re = s_fft_complex[k * 2 + 0];
            float im = s_fft_complex[k * 2 + 1];
            float power = (re * re) + (im * im);
            if (power < 1e-12f) {
                power = 1e-12f;
            }

            float db = 10.0f * log10f(power / (float)FFT_ANALYSIS_SIZE);
            if (db > peak_db) {
                peak_db = db;
            }
        }

        float norm = (peak_db - FFT_DB_FLOOR) / db_span;
        if (norm < 0.0f) norm = 0.0f;
        if (norm > 1.0f) norm = 1.0f;
        bins_local[b] = norm;
    }

    if (xSemaphoreTake(pipeline->mutex, 0) == pdTRUE) {
        memcpy(pipeline->fft_bins, bins_local, sizeof(pipeline->fft_bins));
        pipeline->fft_valid = true;
        xSemaphoreGive(pipeline->mutex);
    }
}

esp_err_t adf_pipeline_get_fft_bins_impl(adf_pipeline_handle_t pipeline,
                                         float *bins_out,
                                         size_t bin_count,
                                         bool *valid_out)
{
    if (!pipeline || !bins_out || bin_count < FFT_PORTAL_BIN_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(pipeline->mutex, portMAX_DELAY);
    memcpy(bins_out, pipeline->fft_bins, sizeof(float) * FFT_PORTAL_BIN_COUNT);
    if (valid_out) {
        *valid_out = pipeline->fft_valid;
    }
    xSemaphoreGive(pipeline->mutex);

    return ESP_OK;
}
