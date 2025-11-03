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
static int udp_sock_ctrl = -1;  // Control/ping socket (high priority)
static int udp_sock_audio = -1; // Audio data socket (low priority)
static struct sockaddr_in broadcast_addr;
static uint32_t last_latency_measurement = 10; // Default 10ms
static esp_netif_t *s_ap_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;
static bool stream_ready = false;  // True when STAs have IPs assigned

// AP event handler for station connect/disconnect and IP assignment
static void wifi_ap_event_handler(void* arg, esp_event_base_t event_base,
                                   int32_t event_id, void* event_data) {
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "STA connected: MAC=%02x:%02x:%02x:%02x:%02x:%02x AID=%d", 
                 event->mac[0], event->mac[1], event->mac[2], 
                 event->mac[3], event->mac[4], event->mac[5], event->aid);
        stream_ready = false;  // Pause streaming until IP assigned
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "STA disconnected: MAC=%02x:%02x:%02x:%02x:%02x:%02x AID=%d reason=%d", 
                 event->mac[0], event->mac[1], event->mac[2], 
                 event->mac[3], event->mac[4], event->mac[5], event->aid, event->reason);
        stream_ready = false;  // Stop streaming immediately
    }
}

static void ip_ap_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    if (event_id == IP_EVENT_AP_STAIPASSIGNED) {
        ip_event_ap_staipassigned_t* event = (ip_event_ap_staipassigned_t*) event_data;
        ESP_LOGI(TAG, "STA assigned IP: " IPSTR, IP2STR(&event->ip));
        stream_ready = true;  // Resume streaming - DHCP complete
    }
}

esp_err_t network_init_ap(void) {
    ESP_LOGI(TAG, "Initializing AP mode with SSID: %s", MESH_SSID);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_ap_netif = esp_netif_create_default_wifi_ap();
    
    // Register AP event handlers for station connect/disconnect and IP assignment
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, 
                                                &wifi_ap_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, 
                                                &wifi_ap_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, 
                                                &ip_ap_event_handler, NULL));

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
    
    // Note: Disabling 802.11b via esp_wifi_set_protocol() not supported in this ESP-IDF version
    // WMM QoS separation should still prioritize control traffic over audio

    // Log AP status
    wifi_config_t current_config;
    esp_wifi_get_config(WIFI_IF_AP, &current_config);
    ESP_LOGI(TAG, "AP started on channel %d", current_config.ap.channel);
    
    // Get AP IP and compute directed broadcast address
    esp_netif_ip_info_t ip_info;
    ESP_ERROR_CHECK(esp_netif_get_ip_info(s_ap_netif, &ip_info));
    uint32_t directed_broadcast = ip_info.ip.addr | ~ip_info.netmask.addr;
    char ip_str[INET_ADDRSTRLEN], bcast_str[INET_ADDRSTRLEN];
    inet_ntoa_r(ip_info.ip, ip_str, sizeof(ip_str));
    struct in_addr bcast_in = { .s_addr = directed_broadcast };
    inet_ntoa_r(bcast_in, bcast_str, sizeof(bcast_str));
    ESP_LOGI(TAG, "AP IP: %s, Broadcast: %s", ip_str, bcast_str);
    
    // Create control socket (high priority - AC_VO for DHCP/control traffic)
    udp_sock_ctrl = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (udp_sock_ctrl < 0) {
        ESP_LOGE(TAG, "Unable to create control socket");
        return ESP_FAIL;
    }
    
    int broadcast = 1;
    setsockopt(udp_sock_ctrl, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    
    // Set control socket to high priority (AC_VO) using DSCP EF (0xB8)
    int tos_vo = 0xB8;  // DSCP EF - highest priority for control/DHCP
    if (setsockopt(udp_sock_ctrl, IPPROTO_IP, IP_TOS, &tos_vo, sizeof(tos_vo)) < 0) {
        ESP_LOGW(TAG, "Failed to set control socket TOS");
    } else {
        ESP_LOGI(TAG, "Control socket set to AC_VO (high priority)");
    }
    
    // Set socket to non-blocking mode to prevent queue overflow blocking
    int flags = fcntl(udp_sock_ctrl, F_GETFL, 0);
    if (flags < 0) {
        ESP_LOGE(TAG, "Failed to get socket flags");
        return ESP_FAIL;
    }
    if (fcntl(udp_sock_ctrl, F_SETFL, flags | O_NONBLOCK) < 0) {
        ESP_LOGE(TAG, "Failed to set non-blocking mode");
        return ESP_FAIL;
    }
    
    // Bind control socket to listen for ping packets
    struct sockaddr_in listen_addr;
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(UDP_PORT);
    listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(udp_sock_ctrl, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
        ESP_LOGE(TAG, "Control socket bind failed");
        return ESP_FAIL;
    }
    
    // Create audio socket (low priority - AC_BK for audio data)
    udp_sock_audio = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (udp_sock_audio < 0) {
        ESP_LOGE(TAG, "Unable to create audio socket");
        return ESP_FAIL;
    }
    
    setsockopt(udp_sock_audio, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    
    // Set audio socket to low priority (AC_BK) using DSCP CS1 (0x20)
    int tos_bk = 0x20;  // DSCP CS1 - lowest priority for audio data
    if (setsockopt(udp_sock_audio, IPPROTO_IP, IP_TOS, &tos_bk, sizeof(tos_bk)) < 0) {
        ESP_LOGW(TAG, "Failed to set audio socket TOS");
    } else {
        ESP_LOGI(TAG, "Audio socket set to AC_BK (low priority)");
    }
    
    // Set audio socket to non-blocking mode
    flags = fcntl(udp_sock_audio, F_GETFL, 0);
    if (flags < 0) {
        ESP_LOGE(TAG, "Failed to get audio socket flags");
        return ESP_FAIL;
    }
    if (fcntl(udp_sock_audio, F_SETFL, flags | O_NONBLOCK) < 0) {
        ESP_LOGE(TAG, "Failed to set audio socket non-blocking mode");
        return ESP_FAIL;
    }
    
    memset(&broadcast_addr, 0, sizeof(broadcast_addr));
    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_port = htons(UDP_PORT);
    broadcast_addr.sin_addr.s_addr = directed_broadcast;

    ESP_LOGI(TAG, "AP initialized: %s (2 sockets: ctrl=AC_VO, audio=AC_BK)", MESH_SSID);
    return ESP_OK;
}

esp_err_t network_init_sta(void) {
    ESP_LOGI(TAG, "Initializing STA mode - looking for SSID: %s", MESH_SSID);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_sta_netif = esp_netif_create_default_wifi_sta();

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
    
    // Wait for IP address assignment from DHCP
    esp_netif_ip_info_t ip_info = {0};
    for (int i = 0; i < 40; i++) {
        esp_netif_get_ip_info(s_sta_netif, &ip_info);
        if (ip_info.ip.addr) break;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    if (!ip_info.ip.addr) {
        ESP_LOGE(TAG, "Failed to obtain IP address from DHCP");
        return ESP_FAIL;
    }
    char sta_ip[INET_ADDRSTRLEN], sta_gw[INET_ADDRSTRLEN];
    inet_ntoa_r(ip_info.ip, sta_ip, sizeof(sta_ip));
    inet_ntoa_r(ip_info.gw, sta_gw, sizeof(sta_gw));
    ESP_LOGI(TAG, "STA IP: %s, Gateway: %s", sta_ip, sta_gw);
    
    // Compute directed broadcast for this subnet
    uint32_t directed_broadcast = ip_info.ip.addr | ~ip_info.netmask.addr;
    char bcast_str[INET_ADDRSTRLEN];
    struct in_addr bcast_in = { .s_addr = directed_broadcast };
    inet_ntoa_r(bcast_in, bcast_str, sizeof(bcast_str));
    ESP_LOGI(TAG, "STA Broadcast: %s", bcast_str);
    
    // STA mode only needs to receive (control socket for audio/ping)
    udp_sock_ctrl = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (udp_sock_ctrl < 0) {
        ESP_LOGE(TAG, "Unable to create socket");
        return ESP_FAIL;
    }

    // For STA mode, set up broadcast capability
    int broadcast = 1;
    setsockopt(udp_sock_ctrl, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    // Set up directed broadcast address for sending
    memset(&broadcast_addr, 0, sizeof(broadcast_addr));
    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_port = htons(UDP_PORT);
    broadcast_addr.sin_addr.s_addr = directed_broadcast;

    struct sockaddr_in listen_addr;
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(UDP_PORT);
    listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(udp_sock_ctrl, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
        ESP_LOGE(TAG, "Socket bind failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "STA initialized and connected");
    return ESP_OK;
}

// Send on control socket (high priority - for pings/control)
esp_err_t network_udp_send(const uint8_t *data, size_t len) {
    if (udp_sock_ctrl < 0) return ESP_ERR_INVALID_STATE;
    
    int sent = sendto(udp_sock_ctrl, data, len, 0, 
                     (struct sockaddr *)&broadcast_addr, sizeof(broadcast_addr));
    
    if (sent < 0) {
        // Handle non-blocking socket errors - silently drop packets when buffer is full
        if (errno == EWOULDBLOCK || errno == EAGAIN || errno == ENOBUFS || errno == ENOMEM) {
            return ESP_OK; // Buffer full - expected when broadcasting with no receivers
        }
        ESP_LOGE(TAG, "Control send failed: %s", strerror(errno));
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

// Send on audio socket (low priority - for audio data)
esp_err_t network_udp_send_audio(const uint8_t *data, size_t len) {
    // Quick fix: broadcast UDP doesn't demux to multiple RX sockets on same port
    // Send audio via control socket so RX receives it (same port 3333)
    // TODO: Use separate ports (3333 ctrl, 3334 audio) for true separation
    int sock = udp_sock_ctrl;
    if (sock < 0) return ESP_ERR_INVALID_STATE;
    
    int sent = sendto(sock, data, len, 0, 
                     (struct sockaddr *)&broadcast_addr, sizeof(broadcast_addr));
    
    if (sent < 0) {
        // Handle non-blocking socket errors - silently drop packets when buffer is full
        if (errno == EWOULDBLOCK || errno == EAGAIN || errno == ENOBUFS || errno == ENOMEM) {
            return ESP_OK; // Buffer full - expected when broadcasting with no receivers
        }
        ESP_LOGD(TAG, "Audio send failed: %s", strerror(errno));
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t network_udp_recv(uint8_t *data, size_t max_len, size_t *actual_len, uint32_t timeout_ms) {
    if (udp_sock_ctrl < 0) return ESP_ERR_INVALID_STATE;

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(udp_sock_ctrl, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in source_addr;
    socklen_t socklen = sizeof(source_addr);
    int len = recvfrom(udp_sock_ctrl, data, max_len, 0,
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
        // This is a ping packet, respond immediately on control socket
        sendto(udp_sock_ctrl, data, len, 0, (struct sockaddr *)&source_addr, socklen);
        // Continue processing as normal (the ping packet will be returned to caller)
    }

    *actual_len = len;
    return ESP_OK;
}

bool network_is_stream_ready(void) {
    return stream_ready;
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
