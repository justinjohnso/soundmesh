/* Minimal UDP receiver (WiFi STA connect to AP) for fast MVP
   - Connects to SSID set by TX SoftAP and listens for UDP packets on port 3333
   - Prints packet sizes; a follow-up can route samples to I2S for audio playback
*/

#include <string.h>
#include <sys/socket.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"

static const char *TAG = "udp_rx";

#define RX_SSID "MeshAudioAP"
#define RX_PASS "meshpass123"
#define UDP_PORT 3333

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    }
}

static void start_sta(void)
{
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {0};
    strcpy((char *)wifi_config.sta.ssid, RX_SSID);
    strcpy((char *)wifi_config.sta.password, RX_PASS);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Started STA and attempting to connect to '%s'", RX_SSID);
}

static void udp_receive_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in local_addr;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(UDP_PORT);

    if (bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
        ESP_LOGE(TAG, "Socket bind failed: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    const int rx_len = 2048;
    uint8_t *rx_buf = malloc(rx_len);
    if (!rx_buf) {
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        int len = recv(sock, rx_buf, rx_len, 0);
        if (len < 0) {
            ESP_LOGW(TAG, "recv failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        ESP_LOGI(TAG, "Received UDP packet, %d bytes", len);
        // TODO: push samples to I2S for audio playback
    }

    free(rx_buf);
    close(sock);
    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    start_sta();

    xTaskCreate(udp_receive_task, "udp_rx", 8192, NULL, 5, NULL);
}
