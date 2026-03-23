#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UPLINK_SSID_MAX_LEN 32
#define UPLINK_PASSWORD_MAX_LEN 64
#define UPLINK_CTRL_VERSION 1
#define UPLINK_CTRL_FLAG_ENABLED 0x01

typedef enum {
    UPLINK_CTRL_SET = 1,
    UPLINK_CTRL_CLEAR = 2,
    UPLINK_CTRL_SYNC = 3,
    UPLINK_CTRL_REQUEST_SYNC = 4
} uplink_ctrl_subtype_t;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t version;
    uint8_t subtype;
    uint8_t flags;
    uint8_t ssid_len;
    uint8_t password_len;
    char ssid[UPLINK_SSID_MAX_LEN + 1];
    char password[UPLINK_PASSWORD_MAX_LEN + 1];
} uplink_ctrl_packet_t;

typedef struct {
    uplink_ctrl_subtype_t subtype;
    bool enabled;
    char ssid[UPLINK_SSID_MAX_LEN + 1];
    char password[UPLINK_PASSWORD_MAX_LEN + 1];
} uplink_ctrl_message_t;

bool uplink_ctrl_encode(const uplink_ctrl_message_t *msg, uplink_ctrl_packet_t *packet);
bool uplink_ctrl_decode(const uplink_ctrl_packet_t *packet, size_t packet_len, uplink_ctrl_message_t *msg);
