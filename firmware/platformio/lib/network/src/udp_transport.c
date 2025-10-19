#include "network/transport.h"
#include "common/config.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "udp_transport";

static transport_role_t current_role = TRANSPORT_ROLE_TX;
static int sock_fd = -1;
static struct sockaddr_in dest_addr;
static transport_stats_t stats = {0};
static bool initialized = false;

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_AP_STACONNECTED:
                ESP_LOGI(TAG, "Station connected to AP");
                break;
            case WIFI_EVENT_AP_STADISCONNECTED:
                ESP_LOGI(TAG, "Station disconnected from AP");
                break;
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi STA started, connecting...");
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "Connected to AP");
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGI(TAG, "Disconnected from AP, reconnecting...");
                esp_wifi_connect();
                break;
            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static esp_err_t init_wifi_ap(const transport_config_t *cfg) {
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t ap_config = {
        .ap = {
            .ssid_len = strlen(cfg->ssid),
            .channel = cfg->channel,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .max_connection = 4,
        },
    };
    strncpy((char*)ap_config.ap.ssid, cfg->ssid, sizeof(ap_config.ap.ssid));
    strncpy((char*)ap_config.ap.password, cfg->password, sizeof(ap_config.ap.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi AP started: SSID=%s, Channel=%d", cfg->ssid, cfg->channel);
    return ESP_OK;
}

static esp_err_t init_wifi_sta(const transport_config_t *cfg) {
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t sta_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char*)sta_config.sta.ssid, cfg->ssid, sizeof(sta_config.sta.ssid));
    strncpy((char*)sta_config.sta.password, cfg->password, sizeof(sta_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi STA connecting to: SSID=%s", cfg->ssid);
    return ESP_OK;
}

static esp_err_t udp_init(const transport_config_t *cfg) {
    if (initialized) {
        ESP_LOGW(TAG, "Transport already initialized");
        return ESP_OK;
    }

    current_role = cfg->role;
    memset(&stats, 0, sizeof(stats));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    if (cfg->role == TRANSPORT_ROLE_TX) {
        ESP_ERROR_CHECK(init_wifi_ap(cfg));
    } else {
        ESP_ERROR_CHECK(init_wifi_sta(cfg));
        vTaskDelay(pdMS_TO_TICKS(5000));  // Wait for connection
    }

    sock_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock_fd < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        return ESP_FAIL;
    }

    if (cfg->role == TRANSPORT_ROLE_TX) {
        int broadcast = 1;
        setsockopt(sock_fd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
        
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(cfg->port);
        dest_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    } else {
        struct sockaddr_in listen_addr = {
            .sin_family = AF_INET,
            .sin_port = htons(cfg->port),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        
        int err = bind(sock_fd, (struct sockaddr *)&listen_addr, sizeof(listen_addr));
        if (err < 0) {
            ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
            close(sock_fd);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Socket bound to port %d", cfg->port);
    }

    initialized = true;
    ESP_LOGI(TAG, "UDP transport initialized (role=%s)", 
             cfg->role == TRANSPORT_ROLE_TX ? "TX" : "RX");
    return ESP_OK;
}

static ssize_t udp_send(const void *data, size_t len) {
    if (!initialized || sock_fd < 0) {
        return -1;
    }

    ssize_t sent = sendto(sock_fd, data, len, 0,
                         (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    
    if (sent < 0) {
        stats.errors++;
        ESP_LOGE(TAG, "Send failed: errno %d", errno);
    } else {
        stats.packets_sent++;
        stats.bytes_sent += sent;
    }

    return sent;
}

static ssize_t udp_recv(void *buf, size_t len, uint32_t timeout_ms) {
    if (!initialized || sock_fd < 0) {
        return -1;
    }

    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in source_addr;
    socklen_t socklen = sizeof(source_addr);
    ssize_t received = recvfrom(sock_fd, buf, len, 0,
                               (struct sockaddr *)&source_addr, &socklen);

    if (received < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            stats.errors++;
        }
        return -1;
    }

    stats.packets_received++;
    stats.bytes_received += received;
    return received;
}

static void udp_get_stats(transport_stats_t *out_stats) {
    if (out_stats) {
        memcpy(out_stats, &stats, sizeof(transport_stats_t));
    }
}

static int udp_get_rssi(void) {
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return -100;
}

static void udp_deinit(void) {
    if (sock_fd >= 0) {
        close(sock_fd);
        sock_fd = -1;
    }
    initialized = false;
    ESP_LOGI(TAG, "UDP transport deinitialized");
}

const transport_vtable_t udp_transport = {
    .init = udp_init,
    .send = udp_send,
    .recv = udp_recv,
    .get_stats = udp_get_stats,
    .get_rssi = udp_get_rssi,
    .deinit = udp_deinit,
};
