#include "control/display.h"
#include "config/pins.h"
#include <esp_log.h>
#include <driver/i2c.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

static const char *TAG = "display";

// Forward declarations
static void display_update(void);

// SSD1306 I2C address
#define SSD1306_I2C_ADDR 0x3C

// SSD1306 commands
#define SSD1306_CMD_DISPLAY_OFF 0xAE
#define SSD1306_CMD_DISPLAY_ON 0xAF
#define SSD1306_CMD_SET_CONTRAST 0x81
#define SSD1306_CMD_ENTIRE_DISPLAY_ON_RESUME 0xA4
#define SSD1306_CMD_NORMAL_DISPLAY 0xA6
#define SSD1306_CMD_SET_MULTIPLEX 0xA8
#define SSD1306_CMD_SET_DISPLAY_OFFSET 0xD3
#define SSD1306_CMD_SET_START_LINE 0x40
#define SSD1306_CMD_CHARGE_PUMP 0x8D
#define SSD1306_CMD_MEMORY_MODE 0x20
#define SSD1306_CMD_SEG_REMAP 0xA1
#define SSD1306_CMD_COM_SCAN_DEC 0xC8
#define SSD1306_CMD_SET_COM_PINS 0xDA
#define SSD1306_CMD_SET_PRECHARGE 0xD9
#define SSD1306_CMD_SET_VCOM_DETECT 0xDB
#define SSD1306_CMD_SET_DISPLAY_CLOCK_DIV 0xD5
#define SSD1306_CMD_SET_COLUMN_ADDR 0x21
#define SSD1306_CMD_SET_PAGE_ADDR 0x22

// Display dimensions (from config/pins.h)
#define DISPLAY_PAGES (DISPLAY_HEIGHT / 8)

// Display buffer (1 bit per pixel, organized in pages)
static uint8_t display_buffer[DISPLAY_WIDTH * DISPLAY_PAGES];

// Simple 5x7 font (ASCII 32-126)
static const uint8_t font5x7[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, // (space)
    {0x00, 0x00, 0x5F, 0x00, 0x00}, // !
    {0x00, 0x07, 0x00, 0x07, 0x00}, // "
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, // #
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, // $
    {0x23, 0x13, 0x08, 0x64, 0x62}, // %
    {0x36, 0x49, 0x56, 0x20, 0x50}, // &
    {0x00, 0x08, 0x07, 0x03, 0x00}, // '
    {0x00, 0x1C, 0x22, 0x41, 0x00}, // (
    {0x00, 0x41, 0x22, 0x1C, 0x00}, // )
    {0x2A, 0x1C, 0x7F, 0x1C, 0x2A}, // *
    {0x08, 0x08, 0x3E, 0x08, 0x08}, // +
    {0x00, 0x80, 0x70, 0x30, 0x00}, // ,
    {0x08, 0x08, 0x08, 0x08, 0x08}, // -
    {0x00, 0x00, 0x60, 0x60, 0x00}, // .
    {0x20, 0x10, 0x08, 0x04, 0x02}, // /
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 0
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // 1
    {0x72, 0x49, 0x49, 0x49, 0x46}, // 2
    {0x21, 0x41, 0x49, 0x4D, 0x33}, // 3
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // 4
    {0x27, 0x45, 0x45, 0x45, 0x39}, // 5
    {0x3C, 0x4A, 0x49, 0x49, 0x31}, // 6
    {0x41, 0x21, 0x11, 0x09, 0x07}, // 7
    {0x36, 0x49, 0x49, 0x49, 0x36}, // 8
    {0x46, 0x49, 0x49, 0x29, 0x1E}, // 9
    {0x00, 0x00, 0x14, 0x00, 0x00}, // :
    {0x00, 0x40, 0x34, 0x00, 0x00}, // ;
    {0x00, 0x08, 0x14, 0x22, 0x41}, // <
    {0x14, 0x14, 0x14, 0x14, 0x14}, // =
    {0x00, 0x41, 0x22, 0x14, 0x08}, // >
    {0x02, 0x01, 0x59, 0x09, 0x06}, // ?
    {0x3E, 0x41, 0x5D, 0x59, 0x4E}, // @
    {0x7C, 0x12, 0x11, 0x12, 0x7C}, // A
    {0x7F, 0x49, 0x49, 0x49, 0x36}, // B
    {0x3E, 0x41, 0x41, 0x41, 0x22}, // C
    {0x7F, 0x41, 0x41, 0x41, 0x3E}, // D
    {0x7F, 0x49, 0x49, 0x49, 0x41}, // E
    {0x7F, 0x09, 0x09, 0x09, 0x01}, // F
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, // G
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, // H
    {0x00, 0x41, 0x7F, 0x41, 0x00}, // I
    {0x20, 0x40, 0x41, 0x3F, 0x01}, // J
    {0x7F, 0x08, 0x14, 0x22, 0x41}, // K
    {0x7F, 0x40, 0x40, 0x40, 0x40}, // L
    {0x7F, 0x02, 0x1C, 0x02, 0x7F}, // M
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, // N
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, // O
    {0x7F, 0x09, 0x09, 0x09, 0x06}, // P
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, // Q
    {0x7F, 0x09, 0x19, 0x29, 0x46}, // R
    {0x26, 0x49, 0x49, 0x49, 0x32}, // S
    {0x03, 0x01, 0x7F, 0x01, 0x03}, // T
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, // U
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, // V
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, // W
    {0x63, 0x14, 0x08, 0x14, 0x63}, // X
    {0x03, 0x04, 0x78, 0x04, 0x03}, // Y
    {0x61, 0x59, 0x49, 0x4D, 0x43}, // Z
    {0x00, 0x00, 0x00, 0x00, 0x00}, // [ (91)
    {0x00, 0x00, 0x00, 0x00, 0x00}, // \ (92)
    {0x00, 0x00, 0x00, 0x00, 0x00}, // ] (93)
    {0x00, 0x00, 0x00, 0x00, 0x00}, // ^ (94)
    {0x00, 0x00, 0x00, 0x00, 0x00}, // _ (95)
    {0x00, 0x00, 0x00, 0x00, 0x00}, // ` (96)
    {0x20, 0x54, 0x54, 0x54, 0x78}, // a
    {0x7F, 0x48, 0x44, 0x44, 0x38}, // b
    {0x38, 0x44, 0x44, 0x44, 0x20}, // c
    {0x38, 0x44, 0x44, 0x48, 0x7F}, // d
    {0x38, 0x54, 0x54, 0x54, 0x18}, // e
    {0x08, 0x7E, 0x09, 0x01, 0x02}, // f
    {0x18, 0xA4, 0xA4, 0xA4, 0x7C}, // g
    {0x7F, 0x08, 0x04, 0x04, 0x78}, // h
    {0x00, 0x44, 0x7D, 0x40, 0x00}, // i
    {0x40, 0x80, 0x80, 0x80, 0x7D}, // j
    {0x7F, 0x10, 0x28, 0x44, 0x00}, // k
    {0x00, 0x41, 0x7F, 0x40, 0x00}, // l
    {0x7C, 0x04, 0x18, 0x04, 0x78}, // m
    {0x7C, 0x08, 0x04, 0x04, 0x78}, // n
    {0x38, 0x44, 0x44, 0x44, 0x38}, // o
    {0xFC, 0x24, 0x24, 0x24, 0x18}, // p
    {0x18, 0x24, 0x24, 0x18, 0xFC}, // q
    {0x7C, 0x08, 0x04, 0x04, 0x08}, // r
    {0x48, 0x54, 0x54, 0x54, 0x20}, // s
    {0x04, 0x3F, 0x44, 0x40, 0x20}, // t
    {0x3C, 0x40, 0x40, 0x20, 0x7C}, // u
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, // v
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, // w
    {0x44, 0x28, 0x10, 0x28, 0x44}, // x
    {0x1C, 0xA0, 0xA0, 0xA0, 0x7C}, // y
    {0x44, 0x64, 0x54, 0x4C, 0x44}, // z
};

// I2C write command
static esp_err_t ssd1306_write_command(uint8_t cmd) {
    uint8_t data[2] = {0x00, cmd};  // 0x00 = command mode
    return i2c_master_write_to_device(I2C_MASTER_NUM, SSD1306_I2C_ADDR, data, 2, pdMS_TO_TICKS(100));
}

// I2C write data
static esp_err_t ssd1306_write_data(const uint8_t *data, size_t len) {
    uint8_t *buf = malloc(len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    
    buf[0] = 0x40;  // 0x40 = data mode
    memcpy(&buf[1], data, len);
    
    esp_err_t ret = i2c_master_write_to_device(I2C_MASTER_NUM, SSD1306_I2C_ADDR, buf, len + 1, pdMS_TO_TICKS(100));
    free(buf);
    return ret;
}

// Initialize SSD1306
esp_err_t display_init(void) {
    ESP_LOGI(TAG, "Initializing SSD1306 display...");
    
    // Initialize I2C
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_MASTER_NUM, &i2c_conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_MASTER_NUM, I2C_MODE_MASTER, 0, 0, 0));
    
    // Power-up delay (SSD1306 datasheet recommendation)
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // SSD1306 initialization sequence for 128x32 display
    ssd1306_write_command(SSD1306_CMD_DISPLAY_OFF);
    ssd1306_write_command(SSD1306_CMD_SET_DISPLAY_CLOCK_DIV);
    ssd1306_write_command(0x80);  // Suggested ratio
    ssd1306_write_command(SSD1306_CMD_SET_MULTIPLEX);
    ssd1306_write_command(0x1F);  // 1/32 duty (32 rows)
    ssd1306_write_command(SSD1306_CMD_SET_DISPLAY_OFFSET);
    ssd1306_write_command(0x00);  // No offset
    ssd1306_write_command(SSD1306_CMD_SET_START_LINE | 0x00);
    ssd1306_write_command(SSD1306_CMD_CHARGE_PUMP);
    ssd1306_write_command(0x14);  // Enable charge pump
    ssd1306_write_command(SSD1306_CMD_MEMORY_MODE);
    ssd1306_write_command(0x00);  // Horizontal addressing mode
    ssd1306_write_command(SSD1306_CMD_SEG_REMAP | 0x01);  // Column 127 mapped to SEG0
    ssd1306_write_command(SSD1306_CMD_COM_SCAN_DEC);  // Scan from COM[N-1] to COM0
    ssd1306_write_command(SSD1306_CMD_SET_COM_PINS);
    ssd1306_write_command(0x02);  // COM pins for 32-row display
    ssd1306_write_command(SSD1306_CMD_SET_CONTRAST);
    ssd1306_write_command(0x8F);  // Contrast for 32-row
    ssd1306_write_command(SSD1306_CMD_SET_PRECHARGE);
    ssd1306_write_command(0xF1);
    ssd1306_write_command(SSD1306_CMD_SET_VCOM_DETECT);
    ssd1306_write_command(0x40);
    ssd1306_write_command(SSD1306_CMD_ENTIRE_DISPLAY_ON_RESUME);
    ssd1306_write_command(SSD1306_CMD_NORMAL_DISPLAY);
    ssd1306_write_command(SSD1306_CMD_DISPLAY_ON);
    
    display_clear();
    display_update();  // Initial clear
    
    ESP_LOGI(TAG, "SSD1306 display initialized (128x%d)", DISPLAY_HEIGHT);
    return ESP_OK;
}

// Clear display buffer
void display_clear(void) {
    memset(display_buffer, 0, sizeof(display_buffer));
}

// Update display from buffer (page-by-page to avoid large I2C writes)
static void display_update(void) {
    for (uint8_t page = 0; page < DISPLAY_PAGES; page++) {
        // Set column address range
        ssd1306_write_command(SSD1306_CMD_SET_COLUMN_ADDR);
        ssd1306_write_command(0);  // Start column
        ssd1306_write_command(DISPLAY_WIDTH - 1);  // End column
        
        // Set page address range
        ssd1306_write_command(SSD1306_CMD_SET_PAGE_ADDR);
        ssd1306_write_command(page);  // Start page
        ssd1306_write_command(page);  // End page (same)
        
        // Write one page (128 bytes)
        ssd1306_write_data(&display_buffer[page * DISPLAY_WIDTH], DISPLAY_WIDTH);
    }
}

// Draw a character at x, y (page-based)
static void display_draw_char(uint8_t x, uint8_t page, char c) {
    if (c < 32 || c > 122) c = ' ';  // Limit to our font range
    
    int font_idx = c - 32;
    for (int i = 0; i < 5; i++) {
        if (x + i < DISPLAY_WIDTH && page < DISPLAY_PAGES) {
            display_buffer[(page * DISPLAY_WIDTH) + x + i] = font5x7[font_idx][i];
        }
    }
}

// Draw a string at x, y (page-based)
static void display_draw_string(uint8_t x, uint8_t page, const char *str) {
    while (*str && x < DISPLAY_WIDTH) {
        display_draw_char(x, page, *str);
        x += 6;  // 5 pixels + 1 space
        str++;
    }
}

// Draw a pixel at (x, y)
static void display_draw_pixel(uint8_t x, uint8_t y) {
    if (x >= DISPLAY_WIDTH || y >= DISPLAY_HEIGHT) return;

    uint8_t page = y / 8;
    uint8_t bit = y % 8;

    display_buffer[page * DISPLAY_WIDTH + x] |= (1 << bit);
}

// Render TX display
void display_render_tx(display_view_t view, const tx_status_t *status) {
    display_clear();

    static uint32_t animation_counter = 0;
    animation_counter++;

    if (view == DISPLAY_VIEW_NETWORK) {
    const char *conn_str = status->connected_nodes > 0 ? "Connected" : "Disconnected";
    display_draw_string(0, 0, conn_str);

        char buf[32];
        snprintf(buf, sizeof(buf), "Nodes: %lu", status->connected_nodes);
        display_draw_string(0, 1, buf);

        display_draw_string(0, 2, "Latency: 10 ms");

    display_draw_string(0, 3, "RSSI: -50 dBm");
    } else {
        const char *mode_str = "Unknown";
    if (status->input_mode == INPUT_MODE_TONE) mode_str = "Tone";
    else if (status->input_mode == INPUT_MODE_USB) mode_str = "USB";
    else if (status->input_mode == INPUT_MODE_AUX) mode_str = "Aux";

    char buf[32];
        snprintf(buf, sizeof(buf), "Source: %s", mode_str);
        display_draw_string(0, 0, buf);

        const char *status_str = status->audio_active ? "Playing..." : "Idle...";
        display_draw_string(0, 1, status_str);

        snprintf(buf, sizeof(buf), "Bandwidth: %lu kbps", status->bandwidth_kbps);
        display_draw_string(0, 2, buf);

        if (status->audio_active) {
            // Draw animated waveform
            for (int x = 0; x < DISPLAY_WIDTH; x++) {
                float phase = ((float)x / DISPLAY_WIDTH) * 2.0f * M_PI + (float)(animation_counter % 100) * 0.1f;
                int y = 16 + (int)(sinf(phase) * 10.0f);
                if (y >= 0 && y < DISPLAY_HEIGHT) {
                    display_draw_pixel(x, y);
                }
            }
        } else {
            // Flat line
            int y = 16;
            for (int x = 0; x < DISPLAY_WIDTH; x++) {
                display_draw_pixel(x, y);
            }
        }
    }

    display_update();
}

// Render RX display
void display_render_rx(display_view_t view, const rx_status_t *status) {
display_clear();

static uint32_t animation_counter = 0;
animation_counter++;

if (view == DISPLAY_VIEW_NETWORK) {
const char *conn_str = status->receiving_audio ? "Connected" : "Disconnected";
display_draw_string(0, 0, conn_str);

char buf[32];
    snprintf(buf, sizeof(buf), "Hops: %lu", status->hops);
    display_draw_string(0, 1, buf);

    snprintf(buf, sizeof(buf), "Latency: %lu ms", status->latency_ms);
        display_draw_string(0, 2, buf);

snprintf(buf, sizeof(buf), "RSSI: %d dBm", status->rssi);
display_draw_string(0, 3, buf);
    } else {
display_draw_string(0, 0, "Streaming...");

char buf[32];
snprintf(buf, sizeof(buf), "Bandwidth: %lu kbps", status->bandwidth_kbps);
display_draw_string(0, 2, buf);

if (status->receiving_audio) {
// Draw animated waveform
    for (int x = 0; x < DISPLAY_WIDTH; x++) {
        float phase = ((float)x / DISPLAY_WIDTH) * 2.0f * M_PI + (float)(animation_counter % 100) * 0.1f;
    int y = 16 + (int)(sinf(phase) * 10.0f);
    if (y >= 0 && y < DISPLAY_HEIGHT) {
        display_draw_pixel(x, y);
}
}
} else {
        // Flat line
            int y = 16;
        for (int x = 0; x < DISPLAY_WIDTH; x++) {
                display_draw_pixel(x, y);
            }
        }
    }

    display_update();
}
