/* Minimal UDP sender (WiFi AP + UDP) for fast MVP
   - Acts as transmitter: generates a simple tone buffer and sends UDP packets to any receiver
   - Intentionally minimal: no ADF, no mesh. We'll use SoftAP + UDP to ensure connectivity between devices
*/

#include "sdkconfig.h"
#include <string.h>
#include <math.h>
#ifndef M_PI
// 0 = streaming bar (bottom), 1 = info bar (top)
static volatile int display_mode = 0;

// Audio input mode
typedef enum {
    AUDIO_INPUT_TONE = 0,
    AUDIO_INPUT_AUX = 1,
    AUDIO_INPUT_USB = 2
} audio_input_mode_t;
static volatile audio_input_mode_t audio_mode = AUDIO_INPUT_TONE;
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
        switch (audio_mode) {
            case AUDIO_INPUT_TONE:
                display.println("Streaming: TONE");
                break;
            case AUDIO_INPUT_AUX:
                display.println("Streaming: AUX");
                break;
            case AUDIO_INPUT_USB:
                display.println("Streaming: USB");
                break;
        }
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "lwip/sockets.h"
#include "driver/i2c.h"
#include "driver/gpio.h"

static const char *TAG = "udp_tx";

#define TX_SSID "MeshAudioAP"
#define TX_PASS "meshpass123"
#define TX_CHANNEL 6
#define UDP_PORT 3333

// Tone generation params
#define SAMPLE_RATE 16000
#define TONE_FREQ 440.0f
#define SAMPLES_PER_PACKET 160 // 10ms @ 16kHz

// I2C and OLED configuration
#define I2C_MASTER_SCL_IO 6
#define I2C_MASTER_SDA_IO 5
#define I2C_MASTER_NUM I2C_NUM_0
#define I2C_MASTER_FREQ_HZ 400000
#define OLED_ADDR 0x3C

// Button (mode toggle) -- use a non-UART, non-boot pin. Avoid GPIO1 which is U0TXD.
// Using GPIO4 is safer for button inputs on XIAO-ESP32-S3.
#define BUTTON_GPIO 4
#define BUTTON_DEBOUNCE_MS 50

// 0 = streaming bar (bottom), 1 = info bar (top)
static volatile int display_mode = 0;

// Global variables
static uint32_t packet_count = 0;
static bool wifi_connected = false;
static volatile int rx_node_count = 1; // Simulated RX node count

// Forward declaration
static void update_oled_display();


// Adafruit_SSD1306 integration (C version for ESP-IDF)

#include "Adafruit_SSD1306.h"

// ESP-IDF compatible Adafruit_SSD1306 instance
Adafruit_SSD1306 display;

static void init_oled() {
    ESP_LOGI(TAG, "Initializing OLED display (Adafruit_SSD1306)...");
    if (!display.init(128, 32, I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO, OLED_ADDR)) {
        ESP_LOGE(TAG, "SSD1306 allocation failed");
        return;
    }
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0,0);
    display.println("TX Ready");
    display.display();
}

// Placeholder for new display update logic
static void update_oled_display() {
    display.clearDisplay();
    if (display_mode == 0) {
        // View 0: Audio streaming waveform animation
        display.setCursor(0,0);
        display.println("Streaming...");
        // Simple waveform animation (sine bar)
        int y_base = 16;
        for (int x = 0; x < 128; x++) {
            float phase = ((float)x / 128.0f) * 2.0f * M_PI + (float)(packet_count % 100) * 0.1f;
            int y = y_base + (int)(sin(phase) * 10.0f);
            display.drawPixel(x, y, SSD1306_WHITE);
        }
    } else {
        // View 1: Number of connected RX nodes (simulated)
        display.setCursor(0,0);
        display.print("Receivers: ");
        display.println(rx_node_count);
    }
    display.display();
}
// Simulate RX node count by incrementing every 5 seconds
static void rx_node_sim_task(void *arg) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        rx_node_count++;
        if (rx_node_count > 4) rx_node_count = 1;
        if (display_mode == 1) update_oled_display();
    }
}

// Button polling + debounce (active low)
static void button_task(void *arg) {
    bool last_pressed = false;
    for (;;) {
        int level = gpio_get_level(BUTTON_GPIO);
        bool pressed = (level == 0);
        if (pressed != last_pressed) {
            vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
            level = gpio_get_level(BUTTON_GPIO);
            pressed = (level == 0);
            if (pressed != last_pressed) {
                last_pressed = pressed;
                if (pressed) {
                    display_mode = (display_mode == 0) ? 1 : 0;
                    update_oled_display();
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_AP_START) {
        wifi_connected = true;
        ESP_LOGI(TAG, "SoftAP started");
    }
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
    ESP_LOGI(TAG, "UDP sender task starting...");
    
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
    
    ESP_LOGI(TAG, "UDP socket configured, starting tone generation...");

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
        } else {
            packet_count++;
            if (packet_count % 10 == 0) { // Update display every 10 packets (~100ms)
                update_oled_display();
                ESP_LOGI(TAG, "Sent %lu packets", packet_count);
            }
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

    // Initialize I2C for OLED
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_MASTER_NUM, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0));
    
    // Initialize OLED display
    vTaskDelay(pdMS_TO_TICKS(100)); // Wait for OLED to stabilize
    init_oled();
    
    // Initialize button GPIO
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&btn_cfg);
    xTaskCreate(button_task, "btn_task", 2048, NULL, 3, NULL);

    // Start RX node simulation task
    xTaskCreate(rx_node_sim_task, "rx_node_sim", 1024, NULL, 2, NULL);
    
    start_softap();

    xTaskCreate(udp_sender_task, "udp_sender", 4096, NULL, 5, NULL);
}
