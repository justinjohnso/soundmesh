#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_STA_LIST_MAX 10

typedef struct {
    int8_t rssi;
} wifi_ap_record_t;

typedef struct {
    uint8_t mac[6];
    int8_t rssi;
} wifi_sta_info_t;

typedef struct {
    int num;
    wifi_sta_info_t sta[WIFI_STA_LIST_MAX];
} wifi_sta_list_t;

esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap_info);
esp_err_t esp_wifi_ap_get_sta_list(wifi_sta_list_t *sta_list);

#ifdef __cplusplus
}
#endif
