/* Minimal UDP receiver (WiFi STA + UDP + I2S DAC) for fast MVP
   - Acts as receiver: connects to TX's SoftAP, receives UDP audio packets, outputs to I2S DAC
   - Intentionally minimal: no ADF, no mesh. We'll use WiFi STA + UDP + I2S to validate connectivity
*/

#include "sdkconfig.h"
#include <string.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "nvs_flash.h"

// Use k0i05/esp_ssd1306 library for OLED display
#include "ssd1306.h"

static const char *TAG = "udp_rx";

#define TX_SSID "MeshAudioAP"
#define TX_PASS "meshpass123"
#define UDP_PORT 3333

// I2S pins for UDA1334 DAC
#define I2S_BCK_IO 7
#define I2S_WS_IO 8
#define I2S_DATA_OUT_IO 9

// I2C and OLED configuration
#define I2C_MASTER_SCL_IO 6
#define I2C_MASTER_SDA_IO 5
#define I2C_MASTER_NUM I2C_NUM_0
#define I2C_MASTER_FREQ_HZ 400000

// Button (mode toggle)
#define BUTTON_GPIO 4
#define BUTTON_DEBOUNCE_MS 50

// Display modes: 0 = mesh stats (latency/hops), 1 = streaming visualization
static volatile int display_mode = 0;

// Global variables
static uint32_t packet_count = 0;
static uint32_t audio_packet_count = 0; // Packets with actual audio (not silence)
static volatile bool is_streaming = false; // True if receiving audio (not silence)
static bool wifi_connected = false;
static i2s_chan_handle_t tx_handle = NULL;
static volatile int wifi_rssi = -100; // WiFi signal strength (dBm)
static const int mesh_hops = 1; // Direct connection = 1 hop
static uint32_t frame_counter = 0; // Smooth animation frame counter
static int last_display_mode = -1; // Track mode changes

// OLED display handles
static i2c_master_bus_handle_t i2c_bus = NULL;
static ssd1306_handle_t ssd1306_dev = NULL;

// Forward declaration
static void update_oled_display();

// Minimal 5x7 font bitmap (ASCII 32-126)
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
    draw_text("RX Ready", 0, 0);
    ESP_ERROR_CHECK(ssd1306_display_pages(ssd1306_dev));
}

static void update_oled_display() {
    char buf[32];
    
    // Clear entire display manually when mode changes
    if (display_mode != last_display_mode) {
        for (int y = 0; y < 32; y++) {
            for (int x = 0; x < 128; x++) {
                ssd1306_set_pixel(ssd1306_dev, x, y, true);
            }
        }
        last_display_mode = display_mode;
    }
    
    if (display_mode == 0) {
        // Clear only the text area for mesh stats mode
        for (int y = 0; y < 24; y++) {
            for (int x = 0; x < 128; x++) {
                ssd1306_set_pixel(ssd1306_dev, x, y, true);
            }
        }
        // View 0: WiFi signal strength as "ping" latency (lower is better)
        // Convert dBm to ping-style metric: -30dBm (excellent) = 10ms, -90dBm (poor) = 200ms
        int ping_ms;
        if (wifi_rssi >= -30) {
            ping_ms = 10; // Excellent signal
        } else if (wifi_rssi <= -90) {
            ping_ms = 200; // Very poor signal
        } else {
            // Linear mapping: -30dBm = 10ms, -90dBm = 200ms
            ping_ms = 10 + ((wifi_rssi + 30) * -190 / 60);
        }
        snprintf(buf, sizeof(buf), "Ping: %d ms", ping_ms);
        draw_text(buf, 0, 0);
        snprintf(buf, sizeof(buf), "Hops: %d", mesh_hops);
        draw_text(buf, 0, 8);
        snprintf(buf, sizeof(buf), "WiFi: %s", wifi_connected ? "OK" : "Down");
        draw_text(buf, 0, 16);
    } else {
        // Clear only the waveform area (bottom portion)
        for (int y = 8; y < 32; y++) {
            for (int x = 0; x < 128; x++) {
                ssd1306_set_pixel(ssd1306_dev, x, y, true);
            }
        }
        // View 1: Audio streaming waveform animation
        if (is_streaming) {
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
            draw_text("Waiting...", 0, 0);
            // Draw flat line when not streaming
            for (int x = 0; x < 128; x++) {
                ssd1306_set_pixel(ssd1306_dev, x, 20, false);
            }
        }
    }
    ssd1306_display_pages(ssd1306_dev);
}

// Update WiFi RSSI (signal strength) every 2 seconds
static void wifi_rssi_update_task(void *arg) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        
        // Query WiFi RSSI if connected
        if (wifi_connected) {
            wifi_ap_record_t ap_info;
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                wifi_rssi = ap_info.rssi;
            }
        } else {
            wifi_rssi = -100; // No signal when disconnected
        }
        
        // Update display in mesh stats mode
        if (display_mode == 0) {
            update_oled_display();
        }
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
    if (event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected = false;
        ESP_LOGI(TAG, "WiFi disconnected, reconnecting...");
        esp_wifi_connect();
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    if (event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_connected = true;
    } else if (event_id == IP_EVENT_STA_LOST_IP) {
        ESP_LOGI(TAG, "Lost IP address");
        wifi_connected = false;
    }
}

static void start_wifi_sta(void)
{
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &ip_event_handler, NULL));

    wifi_config_t wifi_config = {0};
    strcpy((char *)wifi_config.sta.ssid, TX_SSID);
    strcpy((char *)wifi_config.sta.password, TX_PASS);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to '%s'...", TX_SSID);
}

static void init_i2s_dac(void)
{
    ESP_LOGI(TAG, "Initializing I2S for UDA1334 DAC...");
    
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCK_IO,
            .ws   = I2S_WS_IO,
            .dout = I2S_DATA_OUT_IO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
    ESP_LOGI(TAG, "I2S DAC initialized");
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

    TickType_t last_audio_time = 0;
    const TickType_t streaming_timeout = pdMS_TO_TICKS(500); // 500ms timeout for streaming status
    
    while (1) {
        int len = recv(sock, rx_buf, rx_len, 0);
        if (len < 0) {
            ESP_LOGW(TAG, "recv failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        packet_count++;
        bool has_audio = false;
        
        // Convert mono to stereo and output to I2S DAC
        if (len == 320) { // 160 samples * 2 bytes per sample
            int16_t *mono_samples = (int16_t*)rx_buf;
            int16_t stereo_buffer[320]; // 160 samples * 2 channels
            
            // Check if packet contains actual audio (not silence)
            const int16_t silence_threshold = 100;
            for (int i = 0; i < 160; i++) {
                if (mono_samples[i] > silence_threshold || mono_samples[i] < -silence_threshold) {
                    has_audio = true;
                    break;
                }
            }
            
            // Convert to stereo
            for (int i = 0; i < 160; i++) {
                stereo_buffer[i * 2] = mono_samples[i];
                stereo_buffer[i * 2 + 1] = mono_samples[i];
            }
            
            size_t bytes_written = 0;
            i2s_channel_write(tx_handle, stereo_buffer, sizeof(stereo_buffer), &bytes_written, portMAX_DELAY);
        }
        
        // Update streaming status based on audio detection
        if (has_audio) {
            audio_packet_count++;
            last_audio_time = xTaskGetTickCount();
            is_streaming = true;
        } else {
            // Check if we've timed out (no audio for streaming_timeout)
            if ((xTaskGetTickCount() - last_audio_time) > streaming_timeout) {
                is_streaming = false;
            }
        }
        
        // Throttle display updates - only update every 50 packets (~500ms) in streaming mode
        if (display_mode == 1 && packet_count % 50 == 0) {
            update_oled_display();
        }
        
        if (packet_count % 100 == 0) {
            ESP_LOGI(TAG, "Received %lu packets (%lu with audio)", packet_count, audio_packet_count);
        }
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

    // Start WiFi RSSI update task with larger stack for display operations
    xTaskCreate(wifi_rssi_update_task, "wifi_rssi_update", 3072, NULL, 2, NULL);

    // Initialize I2S DAC
    init_i2s_dac();
    
    start_wifi_sta();

    xTaskCreate(udp_receive_task, "udp_receiver", 4096, NULL, 5, NULL);
}