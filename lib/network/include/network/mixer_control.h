#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MIXER_CTRL_VERSION 1
#define MIXER_OUT_GAIN_MIN_PCT 0
#define MIXER_OUT_GAIN_MAX_PCT 400

typedef enum {
    MIXER_CTRL_SET = 1,
    MIXER_CTRL_SYNC = 2,
    MIXER_CTRL_REQUEST_SYNC = 3
} mixer_ctrl_subtype_t;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t version;
    uint8_t subtype;
    uint8_t reserved;
    uint16_t out_gain_pct;
} mixer_ctrl_packet_t;

typedef struct {
    mixer_ctrl_subtype_t subtype;
    uint16_t out_gain_pct;
} mixer_ctrl_message_t;

bool mixer_ctrl_encode(const mixer_ctrl_message_t *msg, mixer_ctrl_packet_t *packet);
bool mixer_ctrl_decode(const mixer_ctrl_packet_t *packet, size_t packet_len, mixer_ctrl_message_t *msg);
