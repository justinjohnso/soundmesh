#pragma once

#include "network/mesh_net.h"

#include <esp_err.h>

void mesh_mixer_request_sync_from_root(void);
esp_err_t mesh_mixer_publish_sync(mixer_ctrl_subtype_t subtype);
void mesh_mixer_handle_control(const mixer_ctrl_message_t *msg);
