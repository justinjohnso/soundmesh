#include "mesh/mesh_identity.h"
#include "mesh/mesh_state.h"
#include "network/mesh_net.h"
#include <stdio.h>

void mesh_id_from_string(const char *str, uint8_t *mesh_id) {
    (void)str;
    const char *readable = "MshN48";
    for (int i = 0; i < 6; i++) {
        mesh_id[i] = (uint8_t)readable[i];
    }
}

void derive_src_id(const uint8_t mac[6], char out_src_id[NETWORK_SRC_ID_LEN]) {
    if (!mac || !out_src_id) {
        return;
    }
#if defined(CONFIG_RX_BUILD)
    snprintf(out_src_id, NETWORK_SRC_ID_LEN, "OUT_%02X%02X%02X", mac[3], mac[4], mac[5]);
#else
    snprintf(out_src_id, NETWORK_SRC_ID_LEN, "SRC_%02X%02X%02X", mac[3], mac[4], mac[5]);
#endif
}

const char *network_get_src_id(void) {
    return g_src_id;
}
