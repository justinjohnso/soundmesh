#include "network/mixer_control.h"

#include "network/mesh_net.h"

#include <string.h>

static bool mixer_ctrl_is_valid_subtype(uint8_t subtype)
{
    return subtype == (uint8_t)MIXER_CTRL_SET ||
           subtype == (uint8_t)MIXER_CTRL_SYNC ||
           subtype == (uint8_t)MIXER_CTRL_REQUEST_SYNC;
}

bool mixer_ctrl_encode(const mixer_ctrl_message_t *msg, mixer_ctrl_packet_t *packet) {
    if (!msg || !packet) {
        return false;
    }
    if (!mixer_ctrl_is_valid_subtype((uint8_t)msg->subtype)) {
        return false;
    }
    if (msg->out_gain_pct < MIXER_OUT_GAIN_MIN_PCT || msg->out_gain_pct > MIXER_OUT_GAIN_MAX_PCT) {
        return false;
    }

    memset(packet, 0, sizeof(*packet));
    packet->type = NET_PKT_TYPE_CONTROL;
    packet->version = MIXER_CTRL_VERSION;
    packet->subtype = (uint8_t)msg->subtype;
    packet->out_gain_pct = htons(msg->out_gain_pct);
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
    if (out_gain_pct < MIXER_OUT_GAIN_MIN_PCT || out_gain_pct > MIXER_OUT_GAIN_MAX_PCT) {
        return false;
    }

    memset(msg, 0, sizeof(*msg));
    msg->subtype = (mixer_ctrl_subtype_t)packet->subtype;
    msg->out_gain_pct = out_gain_pct;
    return true;
}
