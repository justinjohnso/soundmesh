/* Minimal UDP sender (WiFi AP + UDP) for fast MVP
   - Acts as transmitter: generates a simple tone buffer and sends UDP packets to any receiver
   - Intentionally minimal: no ADF, no mesh. We'll use SoftAP + UDP to ensure connectivity between devices
*/

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"

static const char *TAG = "udp_tx";

#define TX_SSID "MeshAudioAP"
#define TX_PASS "meshpass123"
#define TX_CHANNEL 6
#define UDP_PORT 3333

// Tone generation params
#define SAMPLE_RATE 16000
#define TONE_FREQ 440.0f
#define SAMPLES_PER_PACKET 160 // 10ms @ 16kHz

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    // no-op for AP mode
}

static void start_softap(void)
{
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {0};
    strcpy((char *)wifi_config.ap.ssid, TX_SSID);
    wifi_config.ap.ssid_len = strlen(TX_SSID);
    strcpy((char *)wifi_config.ap.password, TX_PASS);
    wifi_config.ap.channel = TX_CHANNEL;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Started SoftAP '%s' channel %d", TX_SSID, TX_CHANNEL);
}

static void udp_sender_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(UDP_PORT);

    // allow broadcast
    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    // prepare tone buffer (16-bit PCM little endian)
    int16_t buffer[SAMPLES_PER_PACKET];
    float phase = 0.0f;
    const float phase_inc = 2.0f * M_PI * TONE_FREQ / SAMPLE_RATE;

    while (1) {
        for (int i = 0; i < SAMPLES_PER_PACKET; ++i) {
            buffer[i] = (int16_t)(sinf(phase) * 3000.0f);
            phase += phase_inc;
            if (phase > 2.0f * M_PI) phase -= 2.0f * M_PI;
        }

        int sent = sendto(sock, (const void *)buffer, sizeof(buffer), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (sent < 0) {
            ESP_LOGW(TAG, "sendto failed: errno=%d", errno);
        }

        vTaskDelay(pdMS_TO_TICKS(10)); // ~10ms per packet
    }

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

    start_softap();

    xTaskCreate(udp_sender_task, "udp_sender", 4096, NULL, 5, NULL);
}
