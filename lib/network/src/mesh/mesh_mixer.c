#include "mesh/mesh_mixer.h"
#include "mesh/mesh_state.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "network_mesh";

static void mixer_set_error(const char *err) {
    if (!err || !*err) {
        s_mixer.last_error[0] = '\0';
        return;
    }
    snprintf(s_mixer.last_error, sizeof(s_mixer.last_error), "%s", err);
}

static bool mixer_stream_is_valid(const network_mixer_stream_status_t *stream) {
    if (!stream) {
        return false;
    }
    if (stream->stream_id < MIXER_STREAM_ID_MIN || stream->stream_id > MIXER_STREAM_ID_MAX) {
        return false;
    }
    if (stream->gain_pct > MIXER_STREAM_GAIN_MAX_PCT) {
        return false;
    }
    return true;
}

static bool mixer_status_is_valid(const network_mixer_status_t *mixer) {
    if (!mixer) {
        return false;
    }
    if (mixer->out_gain_pct > MIXER_OUT_GAIN_MAX_PCT) {
        return false;
    }
    if (mixer->stream_count > MIXER_MAX_STREAMS) {
        return false;
    }
    for (uint8_t i = 0; i < mixer->stream_count; i++) {
        if (!mixer_stream_is_valid(&mixer->streams[i])) {
            return false;
        }
    }
    return true;
}

static void mixer_copy_streams(network_mixer_status_t *dst, const network_mixer_status_t *src) {
    if (!dst || !src) {
        return;
    }
    memset(dst->streams, 0, sizeof(dst->streams));
    dst->stream_count = src->stream_count;
    for (uint8_t i = 0; i < src->stream_count; i++) {
        dst->streams[i] = src->streams[i];
    }
}

static esp_err_t mesh_mixer_apply_local(const network_mixer_status_t *next, bool from_root_sync) {
    if (!mixer_status_is_valid(next)) {
        return ESP_ERR_INVALID_ARG;
    }

    s_mixer.pending_apply = true;
    s_mixer.schema_version = MIXER_SCHEMA_VERSION;
    s_mixer.out_gain_pct = next->out_gain_pct;
    mixer_copy_streams(&s_mixer, next);
    s_mixer.updated_ms = (uint32_t)(esp_timer_get_time() / 1000);

    esp_err_t err = ESP_OK;
    if (mixer_apply_callback) {
        err = mixer_apply_callback(&s_mixer);
        if (err != ESP_OK) {
            mixer_set_error("apply_callback_failed");
            return err;
        }
    }


    s_mixer.pending_apply = false;
    s_mixer.applied = (err == ESP_OK);
    if (err == ESP_OK) {
        mixer_set_error("");
    } else {
        mixer_set_error(esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Mixer apply %s gain=%u%% (sync=%d)",
             err == ESP_OK ? "ok" : "failed",
             (unsigned)s_mixer.out_gain_pct,
             from_root_sync ? 1 : 0);
    return err;
}

esp_err_t mesh_mixer_publish_sync(mixer_ctrl_subtype_t subtype) {
    mixer_ctrl_message_t msg = {
        .subtype = subtype,
        .version = MIXER_CTRL_VERSION,
        .out_gain_pct = s_mixer.out_gain_pct,
        .stream_count = s_mixer.stream_count,
    };
    for (uint8_t i = 0; i < s_mixer.stream_count; i++) {
        msg.streams[i].stream_id = s_mixer.streams[i].stream_id;
        msg.streams[i].gain_pct = s_mixer.streams[i].gain_pct;
        msg.streams[i].enabled = s_mixer.streams[i].enabled;
        msg.streams[i].muted = s_mixer.streams[i].muted;
        msg.streams[i].solo = s_mixer.streams[i].solo;
        msg.streams[i].active = s_mixer.streams[i].active;
    }
    mixer_ctrl_packet_t pkt;
    if (!mixer_ctrl_encode(&msg, &pkt)) {
        return ESP_ERR_INVALID_ARG;
    }
    return network_send_control((const uint8_t *)&pkt, sizeof(pkt));
}

void mesh_mixer_request_sync_from_root(void) {
    mixer_ctrl_message_t msg = {
        .subtype = MIXER_CTRL_REQUEST_SYNC,
        .version = MIXER_CTRL_VERSION,
        .out_gain_pct = s_mixer.out_gain_pct,
        .stream_count = s_mixer.stream_count,
    };
    for (uint8_t i = 0; i < s_mixer.stream_count; i++) {
        msg.streams[i].stream_id = s_mixer.streams[i].stream_id;
        msg.streams[i].gain_pct = s_mixer.streams[i].gain_pct;
        msg.streams[i].enabled = s_mixer.streams[i].enabled;
        msg.streams[i].muted = s_mixer.streams[i].muted;
        msg.streams[i].solo = s_mixer.streams[i].solo;
        msg.streams[i].active = s_mixer.streams[i].active;
    }
    mixer_ctrl_packet_t pkt;
    if (!mixer_ctrl_encode(&msg, &pkt)) {
        return;
    }
    esp_err_t err = network_send_control((const uint8_t *)&pkt, sizeof(pkt));
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "Mixer sync request skipped: %s", esp_err_to_name(err));
    }
}

void mesh_mixer_handle_control(const mixer_ctrl_message_t *msg) {
    if (!msg) {
        return;
    }

    if (msg->subtype == MIXER_CTRL_REQUEST_SYNC) {
        if (is_mesh_root) {
            (void)mesh_mixer_publish_sync(MIXER_CTRL_SYNC);
        }
        return;
    }

    network_mixer_status_t next = s_mixer;
    next.schema_version = MIXER_SCHEMA_VERSION;
    next.out_gain_pct = msg->out_gain_pct;
    if (msg->stream_count > 0) {
        next.stream_count = msg->stream_count;
        for (uint8_t i = 0; i < msg->stream_count; i++) {
            next.streams[i].stream_id = msg->streams[i].stream_id;
            next.streams[i].gain_pct = msg->streams[i].gain_pct;
            next.streams[i].enabled = msg->streams[i].enabled;
            next.streams[i].muted = msg->streams[i].muted;
            next.streams[i].solo = msg->streams[i].solo;
            next.streams[i].active = msg->streams[i].active;
        }
    }

    esp_err_t err = mesh_mixer_apply_local(&next, msg->subtype == MIXER_CTRL_SYNC);
    if (is_mesh_root) {
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Mixer apply failed on root: %s", esp_err_to_name(err));
        }
        (void)mesh_mixer_publish_sync(MIXER_CTRL_SYNC);
    }
}

esp_err_t network_register_mixer_apply_callback(network_mixer_apply_callback_t callback) {
    mixer_apply_callback = callback;
    return ESP_OK;
}

esp_err_t network_set_mixer_state(const network_mixer_status_t *mixer_state) {
    if (!mixer_state) {
        return ESP_ERR_INVALID_ARG;
    }

    network_mixer_status_t next = *mixer_state;
    next.schema_version = MIXER_SCHEMA_VERSION;
    if (next.stream_count == 0) {
        next.stream_count = s_mixer.stream_count;
        for (uint8_t i = 0; i < s_mixer.stream_count; i++) {
            next.streams[i] = s_mixer.streams[i];
        }
    }
    if (!mixer_status_is_valid(&next)) {
        return ESP_ERR_INVALID_ARG;
    }

    mixer_ctrl_message_t msg = {
        .subtype = MIXER_CTRL_SET,
        .version = MIXER_CTRL_VERSION,
        .out_gain_pct = next.out_gain_pct,
        .stream_count = next.stream_count,
    };
    for (uint8_t i = 0; i < next.stream_count; i++) {
        msg.streams[i].stream_id = next.streams[i].stream_id;
        msg.streams[i].gain_pct = next.streams[i].gain_pct;
        msg.streams[i].enabled = next.streams[i].enabled;
        msg.streams[i].muted = next.streams[i].muted;
        msg.streams[i].solo = next.streams[i].solo;
        msg.streams[i].active = next.streams[i].active;
    }

    mixer_ctrl_packet_t pkt;
    if (!mixer_ctrl_encode(&msg, &pkt)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t apply_err = mesh_mixer_apply_local(&next, false);
    if (is_mesh_root) {
        if (apply_err != ESP_OK) {
            ESP_LOGW(TAG, "Mixer apply failed on root: %s", esp_err_to_name(apply_err));
        }
        esp_err_t sync_err = mesh_mixer_publish_sync(MIXER_CTRL_SYNC);
        if (sync_err == ESP_ERR_MESH_NO_ROUTE_FOUND) {
            return apply_err == ESP_OK ? ESP_OK : apply_err;
        }
        return sync_err != ESP_OK ? sync_err : apply_err;
    }

    esp_err_t send_err = network_send_control((const uint8_t *)&pkt, sizeof(pkt));
    if (send_err != ESP_OK) {
        return send_err;
    }
    return apply_err;
}

esp_err_t network_set_mixer_config(uint16_t out_gain_pct) {
    if (out_gain_pct > MIXER_OUT_GAIN_MAX_PCT) {
        return ESP_ERR_INVALID_ARG;
    }
    network_mixer_status_t next = s_mixer;
    next.out_gain_pct = out_gain_pct;
    return network_set_mixer_state(&next);
}

esp_err_t network_get_mixer_state(network_mixer_status_t *out) {
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = s_mixer;
    return ESP_OK;
}
