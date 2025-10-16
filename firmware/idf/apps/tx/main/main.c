/* Minimal UDP sender (WiFi AP + UDP) for fast MVP
   - Acts as transmitter: generates a simple tone buffer and sends UDP packets to any receiver
   - Intentionally minimal: no ADF, no mesh. We'll use SoftAP + UDP to ensure connectivity between devices
*/

#include "sdkconfig.h"
#include <string.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
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

// Forward declaration
static void update_oled_display();

// Simple OLED commands for SSD1306 128x32
static esp_err_t i2c_write_oled(uint8_t *data, size_t len) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (OLED_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd, data, len, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static void oled_send_cmd(uint8_t cmd) {
    uint8_t data[2] = {0x00, cmd}; // 0x00 = command mode
    i2c_write_oled(data, 2);
}

// OLED helpers for page-addressing mode
static void oled_set_page(uint8_t page) {
    oled_send_cmd(0xB0 | (page & 0x07));
    oled_send_cmd(0x00); // lower column = 0
    oled_send_cmd(0x10); // higher column = 0
}

static void oled_write_page(uint8_t page, const uint8_t *buf128) {
    oled_set_page(page);
    uint8_t data[129];
    data[0] = 0x40; // data mode
    memcpy(&data[1], buf128, 128);
    i2c_write_oled(data, 129);
}

static void oled_clear_all() {
    uint8_t zero[128];
    memset(zero, 0x00, sizeof(zero));
    for (uint8_t p = 0; p < 4; ++p) {
        oled_write_page(p, zero);
    }
}

static void oled_fill(uint8_t pattern) {
    uint8_t row[128];
    memset(row, pattern, sizeof(row));
    for (uint8_t p = 0; p < 4; ++p) {
        oled_write_page(p, row);
    }
}

static void i2c_scan_log() {
    for (uint8_t addr = 0x03; addr <= 0x77; ++addr) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "I2C device found at 0x%02X", addr);
        }
    }
}

static void init_oled() {
    ESP_LOGI(TAG, "Initializing OLED display...");
    
    // SSD1306 128x32 initialization sequence
    oled_send_cmd(0xAE); // Display off
    oled_send_cmd(0xD5); // Set display clock
    oled_send_cmd(0x80); // Default ratio
    oled_send_cmd(0xA8); // Set multiplex
    oled_send_cmd(0x1F); // 32 rows (0x1F = 31)
    oled_send_cmd(0xD3); // Set display offset
    oled_send_cmd(0x00); // No offset
    oled_send_cmd(0x40); // Set start line
    oled_send_cmd(0x8D); // Charge pump
    oled_send_cmd(0x14); // Enable charge pump
    oled_send_cmd(0x20); // Memory mode
    oled_send_cmd(0x02); // Page addressing mode
    oled_send_cmd(0xA1); // Set segment remap
    oled_send_cmd(0xC8); // Set COM scan direction
    oled_send_cmd(0xDA); // Set COM pins
    oled_send_cmd(0x02); // Sequential, no remap (128x32)
    oled_send_cmd(0x81); // Set contrast
    oled_send_cmd(0x8F); // Contrast value
    oled_send_cmd(0xD9); // Set precharge
    oled_send_cmd(0xF1); // Precharge value
    oled_send_cmd(0xDB); // Set VCOM detect
    oled_send_cmd(0x40); // VCOM level
    oled_send_cmd(0xA4); // Display follows RAM content
    oled_send_cmd(0xA6); // Normal display (not inverted)
    
    // Clear the display (page by page)
    oled_clear_all();
    oled_send_cmd(0xAF); // Display on

    // Diagnostics: scan I2C and flash a test pattern briefly
    i2c_scan_log();
    oled_fill(0xFF); // full white
    vTaskDelay(pdMS_TO_TICKS(200));
    oled_clear_all();

    ESP_LOGI(TAG, "OLED display initialized");
    
    // Show initial "TX" display
    update_oled_display();
}

static void update_oled_display() {
    int step = (int)(packet_count % 100);
    int bar_len = (step * 128) / 100;
    if (bar_len < 2) bar_len = 2;

    uint8_t blank[128];
    memset(blank, 0x00, sizeof(blank));
    uint8_t bar[128];
    memset(bar, 0x00, sizeof(bar));
    for (int i = 0; i < bar_len && i < 128; i++) bar[i] = 0xFF;

    if (display_mode == 0) {
        oled_write_page(0, blank);
        oled_write_page(1, blank);
        oled_write_page(2, bar);
        oled_write_page(3, bar);
    } else {
        oled_write_page(0, bar);
        oled_write_page(1, bar);
        oled_write_page(2, blank);
        oled_write_page(3, blank);
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
    
    start_softap();

    xTaskCreate(udp_sender_task, "udp_sender", 4096, NULL, 5, NULL);
}
