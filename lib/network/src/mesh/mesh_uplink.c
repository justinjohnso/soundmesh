#include "mesh/mesh_uplink.h"
#include "mesh/mesh_state.h"
#include "network/mesh_net.h"
#include <esp_mesh.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "network_mesh";

static void uplink_set_error(const char *err) {
    if (!err || !*err) {
        s_uplink.last_error[0] = '\0';
        return;
    }
    snprintf(s_uplink.last_error, sizeof(s_uplink.last_error), "%s", err);
}

esp_err_t mesh_uplink_apply_router_config(void) {
    if (!is_mesh_root) {
        return ESP_ERR_INVALID_STATE;
    }

    mesh_router_t router = {0};
    if (s_uplink.enabled && s_uplink.configured && s_uplink.ssid[0] != '\0') {
        size_t ssid_len = strnlen(s_uplink.ssid, UPLINK_SSID_MAX_LEN);
        router.ssid_len = (uint8_t)ssid_len;
        memcpy(router.ssid, s_uplink.ssid, ssid_len);
        memcpy(router.password, s_uplink_password, UPLINK_PASSWORD_MAX_LEN);
        router.allow_router_switch = true;
    } else {
        const char *disabled = "MESHNET_DISABLED";
        size_t ssid_len = strlen(disabled);
        router.ssid_len = (uint8_t)ssid_len;
        memcpy(router.ssid, disabled, ssid_len);
        router.password[0] = '\0';
        router.allow_router_switch = false;
    }

    s_uplink.pending_apply = true;
    esp_err_t err = esp_mesh_set_router(&router);
    s_uplink.pending_apply = false;
    s_uplink.root_applied = (err == ESP_OK);
    s_uplink.updated_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (err == ESP_OK) {
        uplink_set_error("");
    } else {
        uplink_set_error(esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "Uplink router apply %s (configured=%d)", err == ESP_OK ? "ok" : "failed", s_uplink.configured);
    return err;
}

esp_err_t mesh_uplink_publish_sync(uplink_ctrl_subtype_t subtype) {
    uplink_ctrl_message_t msg = {
        .subtype = subtype,
        .enabled = s_uplink.enabled,
    };
    snprintf(msg.ssid, sizeof(msg.ssid), "%s", s_uplink.ssid);
    snprintf(msg.password, sizeof(msg.password), "%s", s_uplink_password);

    uplink_ctrl_packet_t pkt;
    if (!uplink_ctrl_encode(&msg, &pkt)) {
        return ESP_ERR_INVALID_ARG;
    }
    return network_send_control((const uint8_t *)&pkt, sizeof(pkt));
}

void mesh_uplink_request_sync_from_root(void) {
    uplink_ctrl_message_t msg = {
        .subtype = UPLINK_CTRL_REQUEST_SYNC,
        .enabled = false,
    };
    uplink_ctrl_packet_t pkt;
    if (!uplink_ctrl_encode(&msg, &pkt)) {
        return;
    }
    esp_err_t err = network_send_control((const uint8_t *)&pkt, sizeof(pkt));
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "Uplink sync request skipped: %s", esp_err_to_name(err));
    }
}

void mesh_uplink_handle_control(const uplink_ctrl_message_t *msg) {
    if (!msg) {
        return;
    }

    if (msg->subtype == UPLINK_CTRL_REQUEST_SYNC) {
        if (is_mesh_root) {
            (void)mesh_uplink_publish_sync(UPLINK_CTRL_SYNC);
        }
        return;
    }

    if (msg->subtype == UPLINK_CTRL_CLEAR) {
        s_uplink.enabled = false;
        s_uplink.configured = false;
        s_uplink.ssid[0] = '\0';
        s_uplink_password[0] = '\0';
    } else {
        s_uplink.enabled = msg->enabled;
        s_uplink.configured = (msg->ssid[0] != '\0');
        snprintf(s_uplink.ssid, sizeof(s_uplink.ssid), "%s", msg->ssid);
        snprintf(s_uplink_password, sizeof(s_uplink_password), "%s", msg->password);
    }
    s_uplink.updated_ms = (uint32_t)(esp_timer_get_time() / 1000);

    if (is_mesh_root) {
        esp_err_t err = mesh_uplink_apply_router_config();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Uplink apply failed: %s", esp_err_to_name(err));
        }
        (void)mesh_uplink_publish_sync(UPLINK_CTRL_SYNC);
    } else {
        s_uplink.root_applied = false;
        s_uplink.pending_apply = false;
        uplink_set_error("");
    }
}

esp_err_t network_set_uplink_config(const char *ssid, const char *password, bool enabled) {
    uplink_ctrl_message_t msg = {
        .subtype = enabled ? UPLINK_CTRL_SET : UPLINK_CTRL_CLEAR,
        .enabled = enabled,
    };

    if (enabled) {
        if (!ssid || ssid[0] == '\0') {
            return ESP_ERR_INVALID_ARG;
        }
        snprintf(msg.ssid, sizeof(msg.ssid), "%s", ssid);
        snprintf(msg.password, sizeof(msg.password), "%s", password ? password : "");
    }

    uplink_ctrl_packet_t pkt;
    if (!uplink_ctrl_encode(&msg, &pkt)) {
        return ESP_ERR_INVALID_ARG;
    }

    mesh_uplink_handle_control(&msg);
    if (!is_mesh_root) {
        return network_send_control((const uint8_t *)&pkt, sizeof(pkt));
    }
    esp_err_t sync_err = mesh_uplink_publish_sync(UPLINK_CTRL_SYNC);
    if (sync_err == ESP_ERR_MESH_NO_ROUTE_FOUND) {
        return ESP_OK;
    }
    return sync_err;
}

esp_err_t network_get_uplink_status(network_uplink_status_t *out) {
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = s_uplink;
    if (is_mesh_root && s_uplink.pending_apply) {
        out->pending_apply = true;
    }
    return ESP_OK;
}
