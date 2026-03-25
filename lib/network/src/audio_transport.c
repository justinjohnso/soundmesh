#include "network/audio_transport.h"

#include "config/build.h"
#include "network/mesh_net.h"

#include <arpa/inet.h>
#include <string.h>

esp_err_t network_send_audio_batch(const uint8_t *opus_batch_payload,
                                   size_t payload_len,
                                   uint16_t seq,
                                   uint32_t timestamp_ms,
                                   uint8_t frame_count,
                                   uint8_t stream_id)
{
    if (!opus_batch_payload || payload_len == 0 || payload_len > MESH_OPUS_BATCH_MAX_BYTES || frame_count == 0 ||
        frame_count > MESH_FRAMES_PER_PACKET) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t cursor = 0;
    uint8_t parsed_frames = 0;
    while (cursor < payload_len) {
        if (cursor + 2 > payload_len) {
            return ESP_ERR_INVALID_ARG;
        }
        uint16_t frame_len = ((uint16_t)opus_batch_payload[cursor] << 8) | opus_batch_payload[cursor + 1];
        cursor += 2;
        if (frame_len == 0 || frame_len > OPUS_MAX_FRAME_BYTES || (cursor + frame_len) > payload_len) {
            return ESP_ERR_INVALID_ARG;
        }
        cursor += frame_len;
        parsed_frames++;
    }
    if (cursor != payload_len || parsed_frames != frame_count) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t packet[MAX_PACKET_SIZE] = {0};
    net_frame_header_t *hdr = (net_frame_header_t *)packet;

    hdr->magic = NET_FRAME_MAGIC;
    hdr->version = NET_FRAME_VERSION;
    hdr->type = NET_PKT_TYPE_AUDIO_OPUS;
    hdr->stream_id = stream_id;
    hdr->seq = htons(seq);
    hdr->timestamp = htonl(timestamp_ms);
    hdr->payload_len = htons((uint16_t)payload_len);
    hdr->ttl = 6;
    hdr->frame_count = frame_count;
    memcpy(hdr->src_id, network_get_src_id(), NETWORK_SRC_ID_LEN);

    memcpy(packet + NET_FRAME_HEADER_SIZE, opus_batch_payload, payload_len);
    return network_send_audio(packet, NET_FRAME_HEADER_SIZE + payload_len);
}
