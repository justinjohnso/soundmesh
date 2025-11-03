#include "network/mesh_net.h"
#include "config/build.h"
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <nvs_flash.h>
#include <lwip/sockets.h>
#include <string.h>
#include <esp_timer.h>
#include <fcntl.h>
#include <errno.h>

static const char *TAG = "network";
static int udp_sock = -1;
static struct sockaddr_in broadcast_addr;
static uint32_t last_latency_measurement = 10; // Default 10ms

esp_err_t network_init_ap(void) {
    ESP_LOGI(TAG, "Initializing AP mode with SSID: %s", MESH_SSID);

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
            .authmode = WIFI_AUTH_WPA2_PSK,
            .channel = 6,  // Fixed channel for better compatibility
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Log AP status
    wifi_config_t current_config;
    esp_wifi_get_config(WIFI_IF_AP, &current_config);
    ESP_LOGI(TAG, "AP started on channel %d", current_config.ap.channel);
    
    udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (udp_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket");
        return ESP_FAIL;
    }
    
    int broadcast = 1;
    setsockopt(udp_sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    
    // Set socket to non-blocking mode to prevent queue overflow blocking
    int flags = fcntl(udp_sock, F_GETFL, 0);
    if (flags < 0) {
        ESP_LOGE(TAG, "Failed to get socket flags");
        return ESP_FAIL;
    }
    if (fcntl(udp_sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        ESP_LOGE(TAG, "Failed to set non-blocking mode");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "UDP socket set to non-blocking mode");
    
    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_port = htons(UDP_PORT);
    broadcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    // Bind to listen for ping packets
    struct sockaddr_in listen_addr;
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(UDP_PORT);
    listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(udp_sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
        ESP_LOGE(TAG, "Socket bind failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "AP initialized: %s", MESH_SSID);
    return ESP_OK;
}

esp_err_t network_init_sta(void) {
    ESP_LOGI(TAG, "Initializing STA mode - looking for SSID: %s", MESH_SSID);

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

    // First scan for available networks to see if our AP is visible
    ESP_LOGI(TAG, "Scanning for available WiFi networks...");
    wifi_scan_config_t scan_config = {
    .ssid = NULL,
    .bssid = NULL,
        .channel = 0,
        .show_hidden = false
    };

    uint16_t number = 20;
    wifi_ap_record_t ap_records[20];
    esp_wifi_scan_start(&scan_config, true);
    esp_wifi_scan_get_ap_records(&number, ap_records);

    bool found_our_ap = false;
    for (int i = 0; i < number; i++) {
        ESP_LOGI(TAG, "Found AP: %s (RSSI: %d)", ap_records[i].ssid, ap_records[i].rssi);
        if (strcmp((char*)ap_records[i].ssid, MESH_SSID) == 0) {
            found_our_ap = true;
            ESP_LOGI(TAG, "Found target AP %s on channel %d", MESH_SSID, ap_records[i].primary);
        }
    }

    if (!found_our_ap) {
        ESP_LOGW(TAG, "Target AP %s not found in scan! Make sure TX is powered on.", MESH_SSID);
    }

    ESP_ERROR_CHECK(esp_wifi_connect());

    ESP_LOGI(TAG, "Attempting to connect to AP...");
    wifi_ap_record_t ap_info;
    int retry = 0;
    while (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(500));
        retry++;
        ESP_LOGI(TAG, "Connection attempt %d/20", retry);
        if (retry > 20) {
            ESP_LOGE(TAG, "Failed to connect to AP after 10 seconds");
            ESP_LOGE(TAG, "Possible issues:");
            ESP_LOGE(TAG, "1. TX device not powered on or not in AP mode");
            ESP_LOGE(TAG, "2. WiFi password incorrect: %s", MESH_PASSWORD);
            ESP_LOGE(TAG, "3. Physical distance too great");
            ESP_LOGE(TAG, "4. Channel mismatch (TX should be on channel 6)");
            return ESP_FAIL;
        }
    }
ESP_LOGI(TAG, "Connected to AP: %s (RSSI: %d dBm)", MESH_SSID, ap_info.rssi);
    
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "WiFi power-save disabled");
    
    udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (udp_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket");
        return ESP_FAIL;
    }

    // For STA mode, set up broadcast capability
    int broadcast = 1;
    setsockopt(udp_sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    // Set up broadcast address for sending (same as AP mode)
    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_port = htons(UDP_PORT);
    broadcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

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
        // Handle non-blocking socket errors - silently drop packets when buffer is full
        if (errno == EWOULDBLOCK || errno == EAGAIN || errno == ENOBUFS || errno == ENOMEM) {
            return ESP_OK; // Buffer full - expected when broadcasting with no receivers
        }
        ESP_LOGE(TAG, "Send failed: %s", strerror(errno));
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

    // Log packet source and size for debugging
    char src_ip[INET_ADDRSTRLEN];
    inet_ntoa_r(source_addr.sin_addr, src_ip, sizeof(src_ip));
    ESP_LOGD(TAG, "Received UDP packet from %s:%d len=%d", src_ip, ntohs(source_addr.sin_port), len);

    // Debug: print first 16 bytes as hex for diagnosis of header framing issues
    char hexbuf[3 * 16 + 1];
    int to_print = len < 16 ? len : 16;
    for (int i = 0; i < to_print; ++i) {
        sprintf(&hexbuf[i * 3], "%02X ", (unsigned char)data[i]);
    }
    hexbuf[to_print * 3] = '\0';
    ESP_LOGD(TAG, "UDP payload head: %s", hexbuf);

    // Handle ping packets (4 bytes) by responding with the same data
    if (len == 4) {
        // This is a ping packet, respond immediately
        sendto(udp_sock, data, len, 0, (struct sockaddr *)&source_addr, socklen);
        // Continue processing as normal (the ping packet will be returned to caller)
    }

    *actual_len = len;
    return ESP_OK;
}

int network_get_rssi(void) {
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);

    if (mode == WIFI_MODE_STA) {
        // STA mode: get RSSI to connected AP
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            ESP_LOGD(TAG, "STA RSSI: %d dBm", ap_info.rssi);
            return ap_info.rssi;
        }
    } else if (mode == WIFI_MODE_AP) {
        // AP mode: return average RSSI of connected stations
        wifi_sta_list_t sta_list;
        if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK && sta_list.num > 0) {
            int total_rssi = 0;
            for (int i = 0; i < sta_list.num; i++) {
                total_rssi += sta_list.sta[i].rssi;
                ESP_LOGD(TAG, "AP station %d RSSI: %d dBm", i, sta_list.sta[i].rssi);
            }
            int avg_rssi = total_rssi / sta_list.num;
            ESP_LOGD(TAG, "AP average RSSI: %d dBm (%d stations)", avg_rssi, sta_list.num);
            return avg_rssi;
        } else {
            ESP_LOGD(TAG, "AP mode: no connected stations");
        }
    }

    ESP_LOGD(TAG, "RSSI unavailable, returning -100");
    return -100;
}

uint32_t network_get_latency_ms(void) {
    return last_latency_measurement;
}

static void network_measure_latency_task(void *pvParameters) {
    const uint32_t PING_INTERVAL_MS = 2000; // Measure every 2 seconds
    const uint32_t PING_TIMEOUT_MS = 100;

    ESP_LOGI(TAG, "Latency measurement task started");
    while (1) {
        // For AP mode, we can't ping ourselves, so simulate latency
        wifi_mode_t mode;
        esp_wifi_get_mode(&mode);

        if (mode == WIFI_MODE_AP) {
            // In AP mode, simulate realistic latency based on WiFi conditions
            last_latency_measurement = 5 + (esp_random() % 10); // 5-15ms
            ESP_LOGD(TAG, "AP mode latency simulated: %lu ms", last_latency_measurement);
        } else {
            // In STA mode, measure actual round-trip time
            uint64_t start_time = esp_timer_get_time();

            // Send a small ping packet (just a timestamp)
            uint32_t ping_data = (uint32_t)(start_time / 1000); // microseconds to milliseconds
            if (network_udp_send((uint8_t*)&ping_data, sizeof(ping_data)) == ESP_OK) {
                // Wait for response with timeout
                uint8_t response_data[4];
                size_t response_len;
                if (network_udp_recv(response_data, sizeof(response_data), &response_len, PING_TIMEOUT_MS) == ESP_OK &&
                    response_len == sizeof(ping_data)) {
                    uint64_t end_time = esp_timer_get_time();
                    uint32_t rtt = (uint32_t)((end_time - start_time) / 1000); // microseconds to milliseconds
                    last_latency_measurement = rtt / 2; // Round trip, so divide by 2

                    // Clamp to reasonable range
                    if (last_latency_measurement < 1) last_latency_measurement = 1;
                    if (last_latency_measurement > 100) last_latency_measurement = 100;
                    ESP_LOGD(TAG, "STA mode latency measured: %lu ms (RTT: %lu ms)", last_latency_measurement, rtt);
                } else {
                    // Timeout - use previous measurement or default
                    if (last_latency_measurement == 0) last_latency_measurement = 10;
                    ESP_LOGD(TAG, "STA mode ping timeout, using: %lu ms", last_latency_measurement);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(PING_INTERVAL_MS));
    }
}

esp_err_t network_start_latency_measurement(void) {
    xTaskCreate(network_measure_latency_task, "latency_measure", 4096, NULL, 5, NULL);
    return ESP_OK;
}

uint32_t network_get_connected_nodes(void) {
    wifi_sta_list_t sta_list;
    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
        return sta_list.num;
    }
    return 0;
}
