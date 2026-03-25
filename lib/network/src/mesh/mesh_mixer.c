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

static esp_err_t mesh_mixer_apply_local(uint16_t out_gain_pct, bool from_root_sync) {
    if (out_gain_pct < MIXER_OUT_GAIN_MIN_PCT || out_gain_pct > MIXER_OUT_GAIN_MAX_PCT) {
        return ESP_ERR_INVALID_ARG;
    }

    s_mixer.pending_apply = true;
    s_mixer.out_gain_pct = out_gain_pct;
    s_mixer.updated_ms = (uint32_t)(esp_timer_get_time() / 1000);

    esp_err_t err = ESP_OK;
    if (mixer_apply_callback) {
        err = mixer_apply_callback(out_gain_pct);
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
             (unsigned)out_gain_pct,
             from_root_sync ? 1 : 0);
    return err;
}

esp_err_t mesh_mixer_publish_sync(mixer_ctrl_subtype_t subtype) {
    mixer_ctrl_message_t msg = {
        .subtype = subtype,
        .out_gain_pct = s_mixer.out_gain_pct,
    };
    mixer_ctrl_packet_t pkt;
    if (!mixer_ctrl_encode(&msg, &pkt)) {
        return ESP_ERR_INVALID_ARG;
    }
    return network_send_control((const uint8_t *)&pkt, sizeof(pkt));
}

void mesh_mixer_request_sync_from_root(void) {
    mixer_ctrl_message_t msg = {
        .subtype = MIXER_CTRL_REQUEST_SYNC,
        .out_gain_pct = s_mixer.out_gain_pct,
    };
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

    esp_err_t err = mesh_mixer_apply_local(msg->out_gain_pct, msg->subtype == MIXER_CTRL_SYNC);
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

esp_err_t network_set_mixer_config(uint16_t out_gain_pct) {
    if (out_gain_pct < MIXER_OUT_GAIN_MIN_PCT || out_gain_pct > MIXER_OUT_GAIN_MAX_PCT) {
        return ESP_ERR_INVALID_ARG;
    }

    mixer_ctrl_message_t msg = {
        .subtype = MIXER_CTRL_SET,
        .out_gain_pct = out_gain_pct,
    };

    mixer_ctrl_packet_t pkt;
    if (!mixer_ctrl_encode(&msg, &pkt)) {
        return ESP_ERR_INVALID_ARG;
    }

    mesh_mixer_handle_control(&msg);
    if (!is_mesh_root) {
        return network_send_control((const uint8_t *)&pkt, sizeof(pkt));
    }
    return ESP_OK;
}

esp_err_t network_get_mixer_status(network_mixer_status_t *out) {
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = s_mixer;
    return ESP_OK;
}
