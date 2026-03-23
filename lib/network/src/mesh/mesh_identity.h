#pragma once

#include <stdint.h>
#include "network/mesh_net.h"

void mesh_id_from_string(const char *str, uint8_t *mesh_id);
void derive_src_id(const uint8_t mac[6], char out_src_id[NETWORK_SRC_ID_LEN]);
const char *network_get_src_id(void);
