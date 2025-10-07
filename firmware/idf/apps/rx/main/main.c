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
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "driver/gpio.h"

static const char *TAG = "udp_rx";

#define RX_SSID "MeshAudioAP"
#define RX_PASS "meshpass123"
#define UDP_PORT 3333

// I2C and OLED configuration
#define I2C_MASTER_SCL_IO 6
#define I2C_MASTER_SDA_IO 5
#define I2C_MASTER_NUM I2C_NUM_0
#define I2C_MASTER_FREQ_HZ 400000
#define OLED_ADDR 0x3C

// PWM Audio configuration
#define PWM_AUDIO_GPIO 1
#define PWM_FREQ 32000  // PWM frequency
#define PWM_RESOLUTION LEDC_TIMER_8_BIT

// Global variables
static uint32_t packet_count = 0;
static bool wifi_connected = false;

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
    oled_send_cmd(0x00); // Horizontal addressing
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
    
    // Clear the display
    oled_send_cmd(0x21); // Set column address
    oled_send_cmd(0x00); // Start column 0
    oled_send_cmd(0x7F); // End column 127
    oled_send_cmd(0x22); // Set page address
    oled_send_cmd(0x00); // Start page 0
    oled_send_cmd(0x03); // End page 3 (4 pages for 32 rows)
    
    // Clear all pixels
    uint8_t clear_data[129];
    clear_data[0] = 0x40; // Data mode
    for (int i = 1; i < 129; i++) {
        clear_data[i] = 0x00;
    }
    for (int page = 0; page < 4; page++) {
        i2c_write_oled(clear_data, 129);
    }
    
    oled_send_cmd(0xAF); // Display on
    
    ESP_LOGI(TAG, "OLED display initialized");
    
    // Show initial "RX" display
    update_oled_display();
}

static void update_oled_display() {
    // Set addressing window
    oled_send_cmd(0x21); // Set column address
    oled_send_cmd(0x00); // Start column 0
    oled_send_cmd(0x7F); // End column 127
    oled_send_cmd(0x22); // Set page address
    oled_send_cmd(0x00); // Start page 0
    oled_send_cmd(0x03); // End page 3
    
    // Page 0: Show "RX" text
    uint8_t data[129];
    data[0] = 0x40; // Data mode
    for (int i = 1; i < 129; i++) data[i] = 0x00; // Clear
    
    // Simple "RX" pattern
    data[16] = 0xFF; data[17] = 0x66; data[18] = 0x3C; data[19] = 0x00; // R
    data[24] = 0xC3; data[25] = 0x3C; data[26] = 0x18; data[27] = 0x3C; data[28] = 0xC3; // X
    
    i2c_write_oled(data, 129);
    
    // Page 1: Packet count bar
    data[0] = 0x40;
    for (int i = 1; i < 129; i++) data[i] = 0x00;
    
    int bar_len = (packet_count % 100) * 120 / 100; // Scale to display width
    for (int i = 4; i < bar_len + 4 && i < 125; i++) {
        data[i] = 0xFF;
    }
    
    i2c_write_oled(data, 129);
    
    // Clear remaining pages
    for (int i = 1; i < 129; i++) data[i] = 0x00;
    i2c_write_oled(data, 129); // Page 2
    i2c_write_oled(data, 129); // Page 3
}

static void init_pwm_audio() {
    // Configure PWM timer
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = PWM_RESOLUTION,
        .freq_hz = PWM_FREQ,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));
    
    // Configure PWM channel
    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = PWM_AUDIO_GPIO,
        .duty = 0,
        .hpoint = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
}

static void output_audio_pwm(int16_t *samples, int count) {
    // Simple PWM audio output - convert 16-bit samples to 8-bit PWM duty
    for (int i = 0; i < count; i++) {
        // Convert signed 16-bit to unsigned 8-bit for PWM
        uint32_t duty = ((uint32_t)(samples[i] + 32768)) >> 8; // Convert to 0-255 range
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        
        // Small delay to approximate sample rate (very crude)
        esp_rom_delay_us(62); // ~16kHz sample rate
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
        wifi_connected = true;
        ESP_LOGI(TAG, "Connected to WiFi");
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
        
        packet_count++;
        ESP_LOGI(TAG, "Received UDP packet, %d bytes", len);
        
        // Output audio via PWM (if we received 320 bytes = 160 samples)
        if (len == 320) {
            output_audio_pwm((int16_t*)rx_buf, 160);
        }
        
        // Update display every 25 packets (~250ms)
        if (packet_count % 25 == 0) {
            update_oled_display();
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
    
    // Initialize PWM for audio output
    init_pwm_audio();
    
    // Initialize OLED display
    vTaskDelay(pdMS_TO_TICKS(100)); // Wait for OLED to stabilize
    init_oled();
    
    start_sta();

    xTaskCreate(udp_receive_task, "udp_rx", 8192, NULL, 5, NULL);
}
