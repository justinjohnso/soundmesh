#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { ROLE_RX=0, ROLE_TX=1 } node_role_t;

typedef struct {
    node_role_t role;
    int stereo;        // 0 mono, 1 stereo
    int bitrate_kbps;  // target
} node_announce_t;

esp_err_t ctrl_plane_init(void);
esp_err_t ctrl_plane_announce(const node_announce_t *ann);
esp_err_t ctrl_plane_request_tx(void);

#ifdef __cplusplus
}
#endif
