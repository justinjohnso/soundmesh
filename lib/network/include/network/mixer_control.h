#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config/build.h"

#define MIXER_CTRL_VERSION MIXER_SCHEMA_VERSION
#define MIXER_OUT_GAIN_MIN_PCT OUT_OUTPUT_GAIN_MIN_PCT
#define MIXER_OUT_GAIN_MAX_PCT OUT_OUTPUT_GAIN_MAX_PCT

#define MIXER_CTRL_STREAM_FLAG_ENABLED 0x01
#define MIXER_CTRL_STREAM_FLAG_MUTED   0x02
#define MIXER_CTRL_STREAM_FLAG_SOLO    0x04
#define MIXER_CTRL_STREAM_FLAG_ACTIVE  0x08

typedef enum {
    MIXER_CTRL_SET = 1,
    MIXER_CTRL_SYNC = 2,
    MIXER_CTRL_REQUEST_SYNC = 3
} mixer_ctrl_subtype_t;

typedef struct __attribute__((packed)) {
    uint8_t stream_id;
    uint8_t flags;
    uint16_t gain_pct;
} mixer_ctrl_stream_entry_t;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t version;
    uint8_t subtype;
    uint8_t flags;
    uint8_t stream_count;
    uint16_t out_gain_pct;
    mixer_ctrl_stream_entry_t streams[MIXER_MAX_STREAMS];
} mixer_ctrl_packet_t;

typedef struct {
    uint8_t stream_id;
    uint16_t gain_pct;
    bool enabled;
    bool muted;
    bool solo;
    bool active;
} mixer_ctrl_stream_t;

typedef struct {
    mixer_ctrl_subtype_t subtype;
    uint8_t version;
    uint16_t out_gain_pct;
    uint8_t stream_count;
    mixer_ctrl_stream_t streams[MIXER_MAX_STREAMS];
} mixer_ctrl_message_t;

bool mixer_ctrl_encode(const mixer_ctrl_message_t *msg, mixer_ctrl_packet_t *packet);
bool mixer_ctrl_decode(const mixer_ctrl_packet_t *packet, size_t packet_len, mixer_ctrl_message_t *msg);
