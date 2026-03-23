#pragma once

#include "network/uplink_control.h"
#include "network/mesh_net.h"
#include <esp_err.h>

esp_err_t mesh_uplink_apply_router_config(void);
esp_err_t mesh_uplink_publish_sync(uplink_ctrl_subtype_t subtype);
void mesh_uplink_request_sync_from_root(void);
void mesh_uplink_handle_control(const uplink_ctrl_message_t *msg);
esp_err_t network_set_uplink_config(const char *ssid, const char *password, bool enabled);
esp_err_t network_get_uplink_status(network_uplink_status_t *out);
