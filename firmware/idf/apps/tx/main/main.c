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
static uint32_t frame_counter = 0; // Smooth animation frame counter
static int last_display_mode = -1; // Track mode changes

// Forward declaration
static void update_oled_display();


// ESP-IDF built-in SSD1306 driver
// Use k0i05/esp_ssd1306 library for OLED display
#include "ssd1306.h"
#include "nvs_flash.h"

static i2c_master_bus_handle_t i2c_bus = NULL;
static ssd1306_handle_t ssd1306_dev = NULL;

// Minimal 5x7 font bitmap (ASCII 32-126)
// Each character is 5 bytes, each byte is a column
static const uint8_t font_5x7[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, // ' ' (32)
    {0x00, 0x00, 0x5F, 0x00, 0x00}, // '!'
    {0x00, 0x07, 0x00, 0x07, 0x00}, // '"'
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, // '#'
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, // '$'
    {0x23, 0x13, 0x08, 0x64, 0x62}, // '%'
    {0x36, 0x49, 0x55, 0x22, 0x50}, // '&'
    {0x00, 0x05, 0x03, 0x00, 0x00}, // '\''
    {0x00, 0x1C, 0x22, 0x41, 0x00}, // '('
    {0x00, 0x41, 0x22, 0x1C, 0x00}, // ')'
    {0x14, 0x08, 0x3E, 0x08, 0x14}, // '*'
    {0x08, 0x08, 0x3E, 0x08, 0x08}, // '+'
    {0x00, 0x50, 0x30, 0x00, 0x00}, // ','
    {0x08, 0x08, 0x08, 0x08, 0x08}, // '-'
    {0x00, 0x60, 0x60, 0x00, 0x00}, // '.'
    {0x20, 0x10, 0x08, 0x04, 0x02}, // '/'
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // '0' (48)
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // '1'
    {0x42, 0x61, 0x51, 0x49, 0x46}, // '2'
    {0x21, 0x41, 0x45, 0x4B, 0x31}, // '3'
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // '4'
    {0x27, 0x45, 0x45, 0x45, 0x39}, // '5'
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, // '6'
    {0x01, 0x71, 0x09, 0x05, 0x03}, // '7'
    {0x36, 0x49, 0x49, 0x49, 0x36}, // '8'
    {0x06, 0x49, 0x49, 0x29, 0x1E}, // '9'
    {0x00, 0x36, 0x36, 0x00, 0x00}, // ':'
    {0x00, 0x56, 0x36, 0x00, 0x00}, // ';'
    {0x08, 0x14, 0x22, 0x41, 0x00}, // '<'
    {0x14, 0x14, 0x14, 0x14, 0x14}, // '='
    {0x00, 0x41, 0x22, 0x14, 0x08}, // '>'
    {0x02, 0x01, 0x51, 0x09, 0x06}, // '?'
    {0x32, 0x49, 0x79, 0x41, 0x3E}, // '@'
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, // 'A' (65)
    {0x7F, 0x49, 0x49, 0x49, 0x36}, // 'B'
    {0x3E, 0x41, 0x41, 0x41, 0x22}, // 'C'
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, // 'D'
    {0x7F, 0x49, 0x49, 0x49, 0x41}, // 'E'
    {0x7F, 0x09, 0x09, 0x09, 0x01}, // 'F'
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, // 'G'
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, // 'H'
    {0x00, 0x41, 0x7F, 0x41, 0x00}, // 'I'
    {0x20, 0x40, 0x41, 0x3F, 0x01}, // 'J'
    {0x7F, 0x08, 0x14, 0x22, 0x41}, // 'K'
    {0x7F, 0x40, 0x40, 0x40, 0x40}, // 'L'
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, // 'M'
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, // 'N'
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, // 'O'
    {0x7F, 0x09, 0x09, 0x09, 0x06}, // 'P'
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, // 'Q'
    {0x7F, 0x09, 0x19, 0x29, 0x46}, // 'R'
    {0x46, 0x49, 0x49, 0x49, 0x31}, // 'S'
    {0x01, 0x01, 0x7F, 0x01, 0x01}, // 'T'
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, // 'U'
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, // 'V'
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, // 'W'
    {0x63, 0x14, 0x08, 0x14, 0x63}, // 'X'
    {0x07, 0x08, 0x70, 0x08, 0x07}, // 'Y'
    {0x61, 0x51, 0x49, 0x45, 0x43}, // 'Z'
    {0x00, 0x7F, 0x41, 0x41, 0x00}, // '['
    {0x02, 0x04, 0x08, 0x10, 0x20}, // '\\'
    {0x00, 0x41, 0x41, 0x7F, 0x00}, // ']'
    {0x04, 0x02, 0x01, 0x02, 0x04}, // '^'
    {0x40, 0x40, 0x40, 0x40, 0x40}, // '_'
    {0x00, 0x01, 0x02, 0x04, 0x00}, // '`'
    {0x20, 0x54, 0x54, 0x54, 0x78}, // 'a' (97)
    {0x7F, 0x48, 0x44, 0x44, 0x38}, // 'b'
    {0x38, 0x44, 0x44, 0x44, 0x20}, // 'c'
    {0x38, 0x44, 0x44, 0x48, 0x7F}, // 'd'
    {0x38, 0x54, 0x54, 0x54, 0x18}, // 'e'
    {0x08, 0x7E, 0x09, 0x01, 0x02}, // 'f'
    {0x0C, 0x52, 0x52, 0x52, 0x3E}, // 'g'
    {0x7F, 0x08, 0x04, 0x04, 0x78}, // 'h'
    {0x00, 0x44, 0x7D, 0x40, 0x00}, // 'i'
    {0x20, 0x40, 0x44, 0x3D, 0x00}, // 'j'
    {0x7F, 0x10, 0x28, 0x44, 0x00}, // 'k'
    {0x00, 0x41, 0x7F, 0x40, 0x00}, // 'l'
    {0x7C, 0x04, 0x18, 0x04, 0x78}, // 'm'
    {0x7C, 0x08, 0x04, 0x04, 0x78}, // 'n'
    {0x38, 0x44, 0x44, 0x44, 0x38}, // 'o'
    {0x7C, 0x14, 0x14, 0x14, 0x08}, // 'p'
    {0x08, 0x14, 0x14, 0x18, 0x7C}, // 'q'
    {0x7C, 0x08, 0x04, 0x04, 0x08}, // 'r'
    {0x48, 0x54, 0x54, 0x54, 0x20}, // 's'
    {0x04, 0x3F, 0x44, 0x40, 0x20}, // 't'
    {0x3C, 0x40, 0x40, 0x20, 0x7C}, // 'u'
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, // 'v'
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, // 'w'
    {0x44, 0x28, 0x10, 0x28, 0x44}, // 'x'
    {0x0C, 0x50, 0x50, 0x50, 0x3C}, // 'y'
    {0x44, 0x64, 0x54, 0x4C, 0x44}, // 'z'
};

static void draw_char(char c, int x, int y) {
    if (c < 32 || c > 122) return;
    const uint8_t *glyph = font_5x7[c - 32];
    for (int col = 0; col < 5; col++) {
        uint8_t line = glyph[col];
        for (int row = 0; row < 7; row++) {
            if (line & (1 << row)) {
                ssd1306_set_pixel(ssd1306_dev, x + col, y + row, false);
            }
        }
    }
}

static void draw_text(const char *str, int x, int y) {
    while (*str) {
        draw_char(*str, x, y);
        x += 6; // 5 pixels + 1 space
        str++;
    }
}

static void init_oled() {
    ESP_LOGI(TAG, "Initializing OLED display (k0i05/esp_ssd1306)...");
    
    // Initialize I2C master bus
    i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_MASTER_NUM,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &i2c_bus));
    
    // Initialize SSD1306 (128x32 panel)
    ssd1306_config_t ssd1306_config = I2C_SSD1306_128x32_CONFIG_DEFAULT;
    ESP_ERROR_CHECK(ssd1306_init(i2c_bus, &ssd1306_config, &ssd1306_dev));
    ESP_ERROR_CHECK(ssd1306_clear_display(ssd1306_dev, false));
    draw_text("TX Ready", 0, 0);
    ESP_ERROR_CHECK(ssd1306_display_pages(ssd1306_dev));
}

static void update_oled_display() {
    char buf[32];
    
    // Only clear display when mode changes
    if (display_mode != last_display_mode) {
        ssd1306_clear_display(ssd1306_dev, false);
        last_display_mode = display_mode;
    }
    
    if (display_mode == 0) {
        // Clear only the waveform area (bottom portion)
        for (int y = 8; y < 32; y++) {
            for (int x = 0; x < 128; x++) {
                ssd1306_set_pixel(ssd1306_dev, x, y, true);
            }
        }
        draw_text("Streaming...", 0, 0);
        
        // Use frame counter for smoother animation
        for (int x = 0; x < 128; x++) {
            float phase = ((float)x / 128.0f) * 2.0f * M_PI + (float)(frame_counter % 100) * 0.1f;
            int y = 20 + (int)(sin(phase) * 8.0f);
            if (y >= 8 && y < 32) {
                ssd1306_set_pixel(ssd1306_dev, x, y, false);
            }
        }
        frame_counter++;
    } else {
        // Clear only the text area for info mode
        for (int y = 0; y < 32; y++) {
            for (int x = 0; x < 128; x++) {
                ssd1306_set_pixel(ssd1306_dev, x, y, true);
            }
        }
        snprintf(buf, sizeof(buf), "Receivers: %d", rx_node_count);
        draw_text(buf, 0, 0);
    }
    ssd1306_display_pages(ssd1306_dev);
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
            update_oled_display();
            if (packet_count % 100 == 0) {
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

    // Initialize OLED display (handles I2C initialization internally)
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
