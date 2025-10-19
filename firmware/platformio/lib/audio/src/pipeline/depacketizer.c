#include "audio/pipeline.h"
#include "esp_log.h"
#include <stdlib.h>

static const char *TAG = "depacketizer";

struct depacketizer_s {
    uint32_t last_sequence;
    uint32_t packet_count;
    uint32_t lost_packets;
};

esp_err_t depacketizer_init(depacketizer_handle_t *out_handle) {
    if (!out_handle) {
        return ESP_ERR_INVALID_ARG;
    }

    depacketizer_handle_t handle = malloc(sizeof(struct depacketizer_s));
    if (!handle) {
        return ESP_ERR_NO_MEM;
    }

    handle->last_sequence = 0;
    handle->packet_count = 0;
    handle->lost_packets = 0;

    *out_handle = handle;
    ESP_LOGI(TAG, "Depacketizer initialized");
    return ESP_OK;
}

esp_err_t depacketizer_process(depacketizer_handle_t handle, const audio_packet_t *packet,
                              int16_t *out_pcm, uint16_t *out_num_samples) {
    if (!handle || !packet || !out_pcm || !out_num_samples) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = packet_decode(packet, out_pcm, out_num_samples);
    if (ret != ESP_OK) {
        return ret;
    }

    if (handle->packet_count > 0) {
        uint32_t expected_seq = handle->last_sequence + 1;
        if (packet->header.sequence != expected_seq) {
            uint32_t lost = packet->header.sequence - expected_seq;
            handle->lost_packets += lost;
            ESP_LOGW(TAG, "Packet loss detected: expected=%ld, got=%ld (%ld lost)",
                    expected_seq, packet->header.sequence, lost);
        }
    }

    handle->last_sequence = packet->header.sequence;
    handle->packet_count++;

    return ESP_OK;
}

void depacketizer_deinit(depacketizer_handle_t handle) {
    if (handle) {
        ESP_LOGI(TAG, "Depacketizer stats: packets=%ld, lost=%ld (%.2f%%)",
                handle->packet_count, handle->lost_packets,
                (float)handle->lost_packets * 100.0f / (float)handle->packet_count);
        free(handle);
    }
}
