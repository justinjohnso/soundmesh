#include "network/mesh_net.h"
#include "config/build.h"
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <nvs_flash.h>
#include <lwip/sockets.h>
#include <string.h>

static const char *TAG = "network";
static int udp_sock = -1;
static struct sockaddr_in broadcast_addr;

esp_err_t network_init_ap(void) {
    ESP_LOGI(TAG, "Initializing AP mode");
    
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    esp_netif_create_default_wifi_ap();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = MESH_SSID,
            .ssid_len = strlen(MESH_SSID),
            .password = MESH_PASSWORD,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK
        },
    };
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (udp_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket");
        return ESP_FAIL;
    }
    
    int broadcast = 1;
    setsockopt(udp_sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    
    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_port = htons(UDP_PORT);
    broadcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    
    ESP_LOGI(TAG, "AP initialized: %s", MESH_SSID);
    return ESP_OK;
}

esp_err_t network_init_sta(void) {
    ESP_LOGI(TAG, "Initializing STA mode");
    
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    esp_netif_create_default_wifi_sta();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = MESH_SSID,
            .password = MESH_PASSWORD,
        },
    };
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());
    
    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    wifi_ap_record_t ap_info;
    int retry = 0;
    while (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100));
        retry++;
        if (retry > 100) {
            ESP_LOGE(TAG, "Failed to connect to AP");
            return ESP_FAIL;
        }
    }
    ESP_LOGI(TAG, "Connected to AP: %s (RSSI: %d)", MESH_SSID, ap_info.rssi);
    
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "WiFi power-save disabled");
    
    udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (udp_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket");
        return ESP_FAIL;
    }
    
    struct sockaddr_in listen_addr;
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(UDP_PORT);
    listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    
    if (bind(udp_sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
        ESP_LOGE(TAG, "Socket bind failed");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "STA initialized and connected");
    return ESP_OK;
}

esp_err_t network_udp_send(const uint8_t *data, size_t len) {
    if (udp_sock < 0) return ESP_ERR_INVALID_STATE;
    
    int sent = sendto(udp_sock, data, len, 0, 
                     (struct sockaddr *)&broadcast_addr, sizeof(broadcast_addr));
    
    if (sent < 0) {
        ESP_LOGE(TAG, "Send failed");
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t network_udp_recv(uint8_t *data, size_t max_len, size_t *actual_len, uint32_t timeout_ms) {
    if (udp_sock < 0) return ESP_ERR_INVALID_STATE;
    
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(udp_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    struct sockaddr_in source_addr;
    socklen_t socklen = sizeof(source_addr);
    int len = recvfrom(udp_sock, data, max_len, 0, 
                      (struct sockaddr *)&source_addr, &socklen);
    
    if (len < 0) {
        *actual_len = 0;
        return ESP_ERR_TIMEOUT;
    }
    
    *actual_len = len;
    return ESP_OK;
}

int network_get_rssi(void) {
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return -100;
}

uint32_t network_get_latency_ms(void) {
    // TODO: Implement ping-based latency measurement
    return 10;
}
