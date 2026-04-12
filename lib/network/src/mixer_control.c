#include "network/mixer_control.h"

#include "network/mesh_net.h"

#include <string.h>

static bool mixer_ctrl_is_valid_subtype(uint8_t subtype)
{
    return subtype == (uint8_t)MIXER_CTRL_SET ||
           subtype == (uint8_t)MIXER_CTRL_SYNC ||
           subtype == (uint8_t)MIXER_CTRL_REQUEST_SYNC;
}

static bool mixer_ctrl_stream_id_is_valid(uint8_t stream_id)
{
    return stream_id >= MIXER_STREAM_ID_MIN && stream_id <= MIXER_STREAM_ID_MAX;
}

static bool mixer_ctrl_stream_gain_is_valid(uint16_t gain_pct)
{
    return gain_pct <= MIXER_STREAM_GAIN_MAX_PCT;
}

bool mixer_ctrl_encode(const mixer_ctrl_message_t *msg, mixer_ctrl_packet_t *packet) {
    if (!msg || !packet) {
        return false;
    }
    if (!mixer_ctrl_is_valid_subtype((uint8_t)msg->subtype)) {
        return false;
    }
    if (msg->version != 0 && msg->version != MIXER_CTRL_VERSION) {
        return false;
    }
    if (msg->out_gain_pct > MIXER_OUT_GAIN_MAX_PCT) {
        return false;
    }
    if (msg->stream_count > MIXER_MAX_STREAMS) {
        return false;
    }
    for (uint8_t i = 0; i < msg->stream_count; i++) {
        const mixer_ctrl_stream_t *stream = &msg->streams[i];
        if (!mixer_ctrl_stream_id_is_valid(stream->stream_id) ||
            !mixer_ctrl_stream_gain_is_valid(stream->gain_pct)) {
            return false;
        }
    }

    memset(packet, 0, sizeof(*packet));
    packet->type = NET_PKT_TYPE_CONTROL;
    packet->version = (msg->version == 0) ? MIXER_CTRL_VERSION : msg->version;
    packet->subtype = (uint8_t)msg->subtype;
    packet->stream_count = msg->stream_count;
    packet->out_gain_pct = htons(msg->out_gain_pct);
    for (uint8_t i = 0; i < msg->stream_count; i++) {
        const mixer_ctrl_stream_t *src = &msg->streams[i];
        mixer_ctrl_stream_entry_t *dst = &packet->streams[i];
        dst->stream_id = src->stream_id;
        dst->gain_pct = htons(src->gain_pct);
        if (src->enabled) {
            dst->flags |= MIXER_CTRL_STREAM_FLAG_ENABLED;
        }
        if (src->muted) {
            dst->flags |= MIXER_CTRL_STREAM_FLAG_MUTED;
        }
        if (src->solo) {
            dst->flags |= MIXER_CTRL_STREAM_FLAG_SOLO;
        }
        if (src->active) {
            dst->flags |= MIXER_CTRL_STREAM_FLAG_ACTIVE;
        }
    }
    return true;
}

bool mixer_ctrl_decode(const mixer_ctrl_packet_t *packet, size_t packet_len, mixer_ctrl_message_t *msg) {
    if (!packet || !msg || packet_len < sizeof(mixer_ctrl_packet_t)) {
        return false;
    }
    if (packet->type != NET_PKT_TYPE_CONTROL || packet->version != MIXER_CTRL_VERSION) {
        return false;
    }
    if (!mixer_ctrl_is_valid_subtype(packet->subtype)) {
        return false;
    }

    uint16_t out_gain_pct = ntohs(packet->out_gain_pct);
    if (out_gain_pct > MIXER_OUT_GAIN_MAX_PCT) {
        return false;
    }
    if (packet->stream_count > MIXER_MAX_STREAMS) {
        return false;
    }

    memset(msg, 0, sizeof(*msg));
    msg->version = packet->version;
    msg->subtype = (mixer_ctrl_subtype_t)packet->subtype;
    msg->out_gain_pct = out_gain_pct;
    msg->stream_count = packet->stream_count;
    for (uint8_t i = 0; i < packet->stream_count; i++) {
        const mixer_ctrl_stream_entry_t *src = &packet->streams[i];
        mixer_ctrl_stream_t *dst = &msg->streams[i];
        dst->stream_id = src->stream_id;
        dst->gain_pct = ntohs(src->gain_pct);
        dst->enabled = (src->flags & MIXER_CTRL_STREAM_FLAG_ENABLED) != 0;
        dst->muted = (src->flags & MIXER_CTRL_STREAM_FLAG_MUTED) != 0;
        dst->solo = (src->flags & MIXER_CTRL_STREAM_FLAG_SOLO) != 0;
        dst->active = (src->flags & MIXER_CTRL_STREAM_FLAG_ACTIVE) != 0;
        if (!mixer_ctrl_stream_id_is_valid(dst->stream_id) ||
            !mixer_ctrl_stream_gain_is_valid(dst->gain_pct)) {
            return false;
        }
    }
    return true;
}
