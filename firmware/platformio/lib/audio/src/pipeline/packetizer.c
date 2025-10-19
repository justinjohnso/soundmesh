#include "audio/pipeline.h"
#include "common/config.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "packetizer";

struct packetizer_s {
    uint16_t samples_per_packet;
    uint32_t sequence;
    uint32_t timestamp_samples;
};

esp_err_t packetizer_init(const packetizer_config_t *config, packetizer_handle_t *out_handle) {
    if (!config || !out_handle) {
        return ESP_ERR_INVALID_ARG;
    }

    packetizer_handle_t handle = malloc(sizeof(struct packetizer_s));
    if (!handle) {
        return ESP_ERR_NO_MEM;
    }

    handle->samples_per_packet = config->samples_per_packet;
    handle->sequence = 0;
    handle->timestamp_samples = 0;

    *out_handle = handle;
    ESP_LOGI(TAG, "Packetizer initialized (%d samples/packet)", config->samples_per_packet);
    return ESP_OK;
}

esp_err_t packetizer_process(packetizer_handle_t handle, const int16_t *pcm_data,
                            uint16_t num_samples, audio_packet_t *out_packet) {
    if (!handle || !pcm_data || !out_packet) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = packet_encode(out_packet, pcm_data, num_samples,
                                 handle->sequence, handle->timestamp_samples);
    if (ret == ESP_OK) {
        handle->sequence++;
        handle->timestamp_samples += num_samples;
    }

    return ret;
}

void packetizer_deinit(packetizer_handle_t handle) {
    if (handle) {
        free(handle);
        ESP_LOGI(TAG, "Packetizer deinitialized");
    }
}
