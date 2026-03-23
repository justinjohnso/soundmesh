#pragma once

#include <esp_mesh.h>
#include "network/mesh_net.h"

void mesh_ping_handle_ping(const mesh_addr_t *from, const mesh_ping_t *ping);
void mesh_ping_handle_pong(const mesh_ping_t *pong);
esp_err_t network_send_ping(void);
esp_err_t network_ping_nearest_child(void);
