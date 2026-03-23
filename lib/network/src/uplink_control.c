#include "network/uplink_control.h"
#include <string.h>

bool uplink_ctrl_encode(const uplink_ctrl_message_t *msg, uplink_ctrl_packet_t *packet) {
    if (!msg || !packet) {
        return false;
    }

    size_t ssid_len = strnlen(msg->ssid, UPLINK_SSID_MAX_LEN);
    size_t password_len = strnlen(msg->password, UPLINK_PASSWORD_MAX_LEN);
    if (ssid_len > UPLINK_SSID_MAX_LEN || password_len > UPLINK_PASSWORD_MAX_LEN) {
        return false;
    }

    memset(packet, 0, sizeof(*packet));
    packet->type = 0x10;  // NET_PKT_TYPE_CONTROL
    packet->version = UPLINK_CTRL_VERSION;
    packet->subtype = (uint8_t)msg->subtype;
    packet->flags = msg->enabled ? UPLINK_CTRL_FLAG_ENABLED : 0;
    packet->ssid_len = (uint8_t)ssid_len;
    packet->password_len = (uint8_t)password_len;
    memcpy(packet->ssid, msg->ssid, ssid_len);
    memcpy(packet->password, msg->password, password_len);
    packet->ssid[ssid_len] = '\0';
    packet->password[password_len] = '\0';
    return true;
}

bool uplink_ctrl_decode(const uplink_ctrl_packet_t *packet, size_t packet_len, uplink_ctrl_message_t *msg) {
    if (!packet || !msg || packet_len < sizeof(uplink_ctrl_packet_t)) {
        return false;
    }
    if (packet->type != 0x10 || packet->version != UPLINK_CTRL_VERSION) {
        return false;
    }
    if (packet->ssid_len > UPLINK_SSID_MAX_LEN || packet->password_len > UPLINK_PASSWORD_MAX_LEN) {
        return false;
    }

    memset(msg, 0, sizeof(*msg));
    msg->subtype = (uplink_ctrl_subtype_t)packet->subtype;
    msg->enabled = (packet->flags & UPLINK_CTRL_FLAG_ENABLED) != 0;
    memcpy(msg->ssid, packet->ssid, packet->ssid_len);
    msg->ssid[packet->ssid_len] = '\0';
    memcpy(msg->password, packet->password, packet->password_len);
    msg->password[packet->password_len] = '\0';
    return true;
}

