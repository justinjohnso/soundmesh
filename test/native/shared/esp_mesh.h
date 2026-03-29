#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t addr[6];
} mesh_addr_t;

typedef struct {
    uint8_t ssid_len;
    uint8_t ssid[32];
    uint8_t password[64];
    bool allow_router_switch;
    uint8_t bssid[6];
} mesh_router_t;

bool esp_mesh_is_root(void);
uint8_t esp_mesh_get_layer(void);
int esp_mesh_get_routing_table_size(void);
esp_err_t esp_mesh_disconnect(void);
esp_err_t esp_mesh_connect(void);
esp_err_t esp_mesh_set_router(const mesh_router_t *router);

#ifdef __cplusplus
}
#endif
