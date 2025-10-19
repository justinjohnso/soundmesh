#include "common/packet.h"
#include "common/config.h"
#include <string.h>

esp_err_t packet_encode(audio_packet_t *pkt, const int16_t *pcm_data,
                       uint16_t num_samples, uint32_t sequence,
                       uint32_t timestamp_samples) {
    if (!pkt || !pcm_data || num_samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Fill header
    pkt->header.magic = AUDIO_PACKET_MAGIC;
    pkt->header.version = AUDIO_PACKET_VERSION;
    pkt->header.payload_type = PAYLOAD_TYPE_PCM16_MONO;
    pkt->header.sequence = sequence;
    pkt->header.sample_rate = AUDIO_SAMPLE_RATE;
    pkt->header.channels = AUDIO_CHANNELS;
    pkt->header.frame_samples = num_samples;
    pkt->header.timestamp_samples = timestamp_samples;
    pkt->header.payload_size = num_samples * sizeof(int16_t);

    // Copy payload
    memcpy(pkt->payload, pcm_data, pkt->header.payload_size);

    return ESP_OK;
}

esp_err_t packet_decode(const audio_packet_t *pkt, int16_t *pcm_data,
                       uint16_t *num_samples) {
    if (!pkt || !pcm_data || !num_samples) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!packet_validate(pkt)) {
        return ESP_ERR_INVALID_ARG;
    }

    *num_samples = pkt->header.frame_samples;
    memcpy(pcm_data, pkt->payload, pkt->header.payload_size);

    return ESP_OK;
}

bool packet_validate(const audio_packet_t *pkt) {
    if (!pkt) {
        return false;
    }

    if (pkt->header.magic != AUDIO_PACKET_MAGIC) {
        return false;
    }

    if (pkt->header.version != AUDIO_PACKET_VERSION) {
        return false;
    }

    if (pkt->header.payload_size != pkt->header.frame_samples * sizeof(int16_t)) {
        return false;
    }

    return true;
}

size_t packet_total_size(uint16_t num_samples) {
    return sizeof(audio_packet_header_t) + (num_samples * sizeof(int16_t));
}
