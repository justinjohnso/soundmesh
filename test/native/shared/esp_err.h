#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_MESH_DISCONNECTED 0x4001
#define ESP_ERR_MESH_NO_ROUTE_FOUND 0x4002

static inline const char *esp_err_to_name(esp_err_t err)
{
    switch (err) {
        case ESP_OK:
            return "ESP_OK";
        case ESP_ERR_INVALID_ARG:
            return "ESP_ERR_INVALID_ARG";
        case ESP_ERR_INVALID_STATE:
            return "ESP_ERR_INVALID_STATE";
        case ESP_ERR_MESH_DISCONNECTED:
            return "ESP_ERR_MESH_DISCONNECTED";
        case ESP_ERR_MESH_NO_ROUTE_FOUND:
            return "ESP_ERR_MESH_NO_ROUTE_FOUND";
        default:
            return "ESP_ERR_UNKNOWN";
    }
}

#ifdef __cplusplus
}
#endif
