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
static volatile audio_input_mode_t audio_mode = AUDIO_INPUT_USB;
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "lwip/sockets.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "ssd1306.h"
#include "nvs_flash.h"

#include "usb_device_uac.h"

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

// PCF8591 ADC configuration
#define PCF8591_ADDR 0x48
#define PCF8591_CHANNEL 0 // AIN0

// Audio buffer configuration
#define AUDIO_BUFFER_SIZE (SAMPLES_PER_PACKET * 4 * sizeof(int16_t)) // 4 packets buffered

// USB Audio configuration
#define USB_AUDIO_SAMPLE_RATE 16000
#define USB_AUDIO_CHANNEL_COUNT 1
#define USB_AUDIO_BIT_RESOLUTION 16
#define USB_AUDIO_SAMPLE_SIZE (USB_AUDIO_BIT_RESOLUTION / 8)
#define USB_AUDIO_EP_SIZE (USB_AUDIO_SAMPLE_RATE / 1000 * USB_AUDIO_CHANNEL_COUNT * USB_AUDIO_SAMPLE_SIZE)

// 0 = streaming bar (bottom), 1 = info bar (top)
static volatile int display_mode = 0;

// Audio input mode
typedef enum {
    AUDIO_INPUT_TONE = 0,
    AUDIO_INPUT_USB = 1,
    AUDIO_INPUT_AUX = 2
} audio_input_mode_t;

// Global variables
static uint32_t packet_count = 0;
static volatile bool is_streaming = false; // True if actually sending audio (not silence)
static bool wifi_connected = false;
static volatile int rx_node_count = 1; // Simulated RX node count
static uint32_t frame_counter = 0; // Smooth animation frame counter
static int last_display_mode = -1; // Track mode changes
static volatile audio_input_mode_t audio_mode = AUDIO_INPUT_USB;
static RingbufHandle_t audio_ring_buffer = NULL;

// Forward declarations
static void update_oled_display();
static void init_usb_audio(void);

// TinyUSB and OLED use driver_ng I2C API
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
    ESP_LOGI(TAG, "Initializing OLED display with driver_ng...");
    
    // Initialize I2C master bus (driver_ng API)
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
        // Clear only the waveform area (bottom portion)
        for (int y = 8; y < 32; y++) {
            for (int x = 0; x < 128; x++) {
                ssd1306_set_pixel(ssd1306_dev, x, y, true);
            }
        }
        
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
    } else {
        // Clear only the text area for info mode
        for (int y = 0; y < 16; y++) {
            for (int x = 0; x < 128; x++) {
                ssd1306_set_pixel(ssd1306_dev, x, y, true);
            }
        }
        // Show audio mode and receiver count
        const char *mode_str = (audio_mode == AUDIO_INPUT_TONE) ? "Tone" :
                               (audio_mode == AUDIO_INPUT_USB) ? "USB" : "AUX";
        snprintf(buf, sizeof(buf), "Input: %s", mode_str);
        draw_text(buf, 0, 0);
        snprintf(buf, sizeof(buf), "RX: %d", rx_node_count);
        draw_text(buf, 0, 8);
    }
    ESP_ERROR_CHECK(ssd1306_display_pages(ssd1306_dev));
}

static void rx_node_sim_task(void *arg) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        rx_node_count++;
        if (rx_node_count > 4) rx_node_count = 1;
        if (display_mode == 1) update_oled_display();
    }
}

// Button polling + debounce (active low)
// Short press (< 1s): Toggle display mode
// Long press (>= 1s): Cycle audio input mode
static void button_task(void *arg) {
    bool last_pressed = false;
    TickType_t press_start_time = 0;
    bool long_press_triggered = false;
    const TickType_t long_press_threshold = pdMS_TO_TICKS(1000); // 1 second
    
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
                    // Button just pressed - record start time
                    press_start_time = xTaskGetTickCount();
                    long_press_triggered = false;
                } else {
                    // Button just released
                    if (!long_press_triggered) {
                        // Short press: Toggle display mode
                        display_mode = (display_mode == 0) ? 1 : 0;
                        update_oled_display();
                    }
                }
            }
        }
        
        // Check for long press while button is held down
        if (pressed && !long_press_triggered) {
            TickType_t press_duration = xTaskGetTickCount() - press_start_time;
            if (press_duration >= long_press_threshold) {
                // Long press detected - trigger mode change immediately
                long_press_triggered = true;
                audio_mode = (audio_mode + 1) % 3;
                const char *mode_str = (audio_mode == AUDIO_INPUT_TONE) ? "Tone" :
                                       (audio_mode == AUDIO_INPUT_USB) ? "USB" : "AUX";
                ESP_LOGI(TAG, "Audio mode: %s", mode_str);
                
                // Show mode on info screen immediately
                int previous_display_mode = display_mode;
                display_mode = 1;
                update_oled_display();
                
                // Wait for button release, then restore previous display
                while (gpio_get_level(BUTTON_GPIO) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
                vTaskDelay(pdMS_TO_TICKS(1500)); // Show for 1.5s after release
                display_mode = previous_display_mode;
                update_oled_display();
                last_pressed = false; // Reset state after handling
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

// ============================================================================
// USB AUDIO CLASS DEVICE (UAC) - Using Espressif's usb_device_uac component
// This makes the ESP32 appear as a USB Audio speaker (audio OUTPUT from computer)
// ============================================================================

// UAC callback: Called when audio data arrives FROM the computer (speaker mode)
static esp_err_t uac_device_output_cb(uint8_t *buf, size_t len, void *arg)
{
    (void)arg;
    
    // If we have a ring buffer and received audio data, write it
    if (audio_ring_buffer != NULL && len > 0 && audio_mode == AUDIO_INPUT_USB) {
        // Write to ring buffer for UDP transmission  
        // Using non-blocking send with short timeout
        BaseType_t result = xRingbufferSend(audio_ring_buffer, buf, len, pdMS_TO_TICKS(5));
        if (result == pdTRUE) {
            is_streaming = true;
        }
    }
    
    return ESP_OK;
}

// Simple scan function for I2C debugging
static void scan_i2c_bus(void) = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x303A, // Espressif VID
    .idProduct          = 0x4002,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

// Configuration Descriptor Length Calculation
// Based on UAC2 speaker device structure
#define TUD_AUDIO_DESC_IAD_LEN 8
#define TUD_AUDIO_DESC_STD_AC_LEN 9
#define TUD_AUDIO_DESC_CS_AC_LEN 9
#define TUD_AUDIO_DESC_CLK_SRC_LEN 8
#define TUD_AUDIO_DESC_INPUT_TERM_LEN 17
#define TUD_AUDIO_DESC_OUTPUT_TERM_LEN 12
#define TUD_AUDIO_DESC_FEATURE_UNIT_ONE_CHANNEL_LEN 14

#define TUD_AUDIO_SPEAKER_CTRL_LEN (TUD_AUDIO_DESC_CLK_SRC_LEN + TUD_AUDIO_DESC_INPUT_TERM_LEN + \
                                     TUD_AUDIO_DESC_OUTPUT_TERM_LEN + TUD_AUDIO_DESC_FEATURE_UNIT_ONE_CHANNEL_LEN)

#define TUD_AUDIO_DESC_STD_AS_INT_LEN 9
#define TUD_AUDIO_DESC_CS_AS_INT_LEN 16
#define TUD_AUDIO_DESC_TYPE_I_FORMAT_LEN 6
#define TUD_AUDIO_DESC_STD_AS_ISO_EP_LEN 7
#define TUD_AUDIO_DESC_CS_AS_ISO_EP_LEN 8

#define TUD_AUDIO_SPEAKER_STREAMING_LEN (2 * TUD_AUDIO_DESC_STD_AS_INT_LEN + TUD_AUDIO_DESC_CS_AS_INT_LEN + \
                                          TUD_AUDIO_DESC_TYPE_I_FORMAT_LEN + TUD_AUDIO_DESC_STD_AS_ISO_EP_LEN + \
                                          TUD_AUDIO_DESC_CS_AS_ISO_EP_LEN)

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_AUDIO_DESC_IAD_LEN + TUD_AUDIO_DESC_STD_AC_LEN + \
                          TUD_AUDIO_DESC_CS_AC_LEN + TUD_AUDIO_SPEAKER_CTRL_LEN + TUD_AUDIO_SPEAKER_STREAMING_LEN)

// USB Audio Configuration Descriptor - Simple speaker (mono, 16kHz, 16-bit)
// Based on UAC2 specification and uac2_headset example structure
static uint8_t const usb_audio_configuration_descriptor[] = {
    // Configuration Descriptor (9 bytes)
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    
    // Interface Association Descriptor (8 bytes)
    8, TUSB_DESC_INTERFACE_ASSOCIATION, ITF_NUM_AUDIO_CONTROL, 2, TUSB_CLASS_AUDIO, AUDIO_SUBCLASS_UNDEFINED, AUDIO_INT_PROTOCOL_CODE_V2, 0,
    
    // Standard AC Interface Descriptor (9 bytes)
    9, TUSB_DESC_INTERFACE, ITF_NUM_AUDIO_CONTROL, 0, 0, TUSB_CLASS_AUDIO, AUDIO_SUBCLASS_CONTROL, AUDIO_INT_PROTOCOL_CODE_V2, 0,
    
    // Class-Specific AC Interface Header Descriptor (9 bytes)
    9, TUSB_DESC_CS_INTERFACE, AUDIO_CS_AC_INTERFACE_HEADER, U16_TO_U8S_LE(0x0200), AUDIO_FUNC_DESKTOP_SPEAKER,
    U16_TO_U8S_LE(TUD_AUDIO_SPEAKER_CTRL_LEN), AUDIO_CS_AS_INTERFACE_CTRL_LATENCY_POS,
    
    // Clock Source Descriptor (8 bytes) - ID 0x04
    8, TUSB_DESC_CS_INTERFACE, AUDIO_CS_AC_INTERFACE_CLOCK_SOURCE, 0x04, AUDIO_CLOCK_SOURCE_ATT_INT_FIX_CLK,
    (AUDIO_CTRL_R << AUDIO_CLOCK_SOURCE_CTRL_CLK_FRQ_POS), 0x00, 0x00,
    
    // Input Terminal Descriptor (17 bytes) - ID 0x01 (USB streaming IN)
    17, TUSB_DESC_CS_INTERFACE, AUDIO_CS_AC_INTERFACE_INPUT_TERMINAL, 0x01, U16_TO_U8S_LE(AUDIO_TERM_TYPE_USB_STREAMING),
    0x00, 0x04, 0x01, U32_TO_U8S_LE(AUDIO_CHANNEL_CONFIG_NON_PREDEFINED), 0x00, U16_TO_U8S_LE(0x0000), 0x00,
    
    // Feature Unit Descriptor (14 bytes) - ID 0x02 (Volume control)
    14, TUSB_DESC_CS_INTERFACE, AUDIO_CS_AC_INTERFACE_FEATURE_UNIT, 0x02, 0x01,
    U32_TO_U8S_LE((AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_MUTE_POS) | (AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_VOLUME_POS)),
    U32_TO_U8S_LE(0x00000000), 0x00,
    
    // Output Terminal Descriptor (12 bytes) - ID 0x03 (Desktop speaker OUT)
    12, TUSB_DESC_CS_INTERFACE, AUDIO_CS_AC_INTERFACE_OUTPUT_TERMINAL, 0x03, U16_TO_U8S_LE(AUDIO_TERM_TYPE_OUT_DESKTOP_SPEAKER),
    0x00, 0x02, 0x04, U16_TO_U8S_LE(0x0000), 0x00,
    
    // Standard AS Interface Descriptor - Alt 0 (Zero Bandwidth) (9 bytes)
    9, TUSB_DESC_INTERFACE, ITF_NUM_AUDIO_STREAMING, 0, 0, TUSB_CLASS_AUDIO, AUDIO_SUBCLASS_STREAMING, AUDIO_INT_PROTOCOL_CODE_V2, 0,
    
    // Standard AS Interface Descriptor - Alt 1 (Operational) (9 bytes)
    9, TUSB_DESC_INTERFACE, ITF_NUM_AUDIO_STREAMING, 1, 1, TUSB_CLASS_AUDIO, AUDIO_SUBCLASS_STREAMING, AUDIO_INT_PROTOCOL_CODE_V2, 0,
    
    // Class-Specific AS Interface Descriptor (16 bytes)
    16, TUSB_DESC_CS_INTERFACE, AUDIO_CS_AS_INTERFACE_AS_GENERAL, 0x01, AUDIO_CTRL_NONE, AUDIO_FORMAT_TYPE_I,
    U32_TO_U8S_LE(AUDIO_DATA_FORMAT_TYPE_I_PCM), 0x01, U32_TO_U8S_LE(AUDIO_CHANNEL_CONFIG_NON_PREDEFINED), 0x00,
    
    // Type I Format Type Descriptor (6 bytes)
    6, TUSB_DESC_CS_INTERFACE, AUDIO_CS_AS_INTERFACE_FORMAT_TYPE, AUDIO_FORMAT_TYPE_I, 0x02, 0x10,
    
    // Standard AS Isochronous Audio Data Endpoint Descriptor (7 bytes)
    7, TUSB_DESC_ENDPOINT, EPNUM_AUDIO_OUT, (TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ADAPTIVE | TUSB_ISO_EP_ATT_DATA),
    U16_TO_U8S_LE(64), 1,
    
    // Class-Specific AS Isochronous Audio Data Endpoint Descriptor (8 bytes)
    8, TUSB_DESC_CS_ENDPOINT, AUDIO_CS_EP_SUBTYPE_GENERAL, AUDIO_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK,
    AUDIO_CTRL_NONE, AUDIO_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, U16_TO_U8S_LE(0x0000),
};

// String Descriptors
static const char* usb_string_descriptors[] = {
    "",                       // 0: Language (filled by TinyUSB)
    "NYU ITP",               // 1: Manufacturer
    "MeshNet Audio TX",      // 2: Product
    "123456",                // 3: Serial Number
    "MeshNet Speakers",      // 4: Audio Interface
};

// ============================================================================
// USB AUDIO CALLBACKS
// ============================================================================

// Buffer for speaker data (receiving audio FROM computer)
static uint8_t spk_buf[512];
static int spk_data_size = 0;

// Called when USB audio data arrives FROM the computer
bool tud_audio_rx_done_post_read_cb(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out, uint8_t cur_alt_setting)
{
    (void)rhport;
    (void)func_id;
    (void)ep_out;
    (void)cur_alt_setting;
    
    // If we have a ring buffer and received audio data, write it
    if (audio_ring_buffer != NULL && n_bytes_received > 0 && audio_mode == AUDIO_INPUT_USB) {
        uint16_t bytes_to_copy = n_bytes_received < sizeof(spk_buf) ? n_bytes_received : sizeof(spk_buf);
        
        // Write to ring buffer for UDP transmission
        xRingbufferSendFromISR(audio_ring_buffer, spk_buf, bytes_to_copy, NULL);
        
        is_streaming = true;
        spk_data_size = bytes_to_copy;
    }
    
    return true;
}

// Interface control callbacks
bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const * p_request)
{
    (void)rhport;
    uint8_t const itf = tu_u16_low(tu_le16toh(p_request->wIndex));
    uint8_t const alt = tu_u16_low(tu_le16toh(p_request->wValue));
    
    ESP_LOGI(TAG, "USB Audio: Set interface %d alt %d", itf, alt);
    
    // Clear buffer when streaming format changes
    spk_data_size = 0;
    
    return true;
}

bool tud_audio_set_itf_close_EP_cb(uint8_t rhport, tusb_control_request_t const * p_request)
{
    (void)rhport;
    (void)p_request;
    return true;
}

// Required entity control callbacks (minimal implementation)
bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    (void)rhport;
    (void)p_request;
    ESP_LOGD(TAG, "USB Audio: Get request entity");
    return false;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *buf)
{
    (void)rhport;
    (void)p_request;
    (void)buf;
    ESP_LOGD(TAG, "USB Audio: Set request entity");
    return false;
}

static void scan_i2c_bus(void)
{
    ESP_LOGI(TAG, "Scanning I2C bus...");
    int devices_found = 0;
    
    // Scan all 7-bit I2C addresses (0x03 to 0x77, skipping reserved)
    for (uint8_t addr = 0x03; addr < 0x78; addr++) {
        // Create temporary device config for probing
        i2c_device_config_t probe_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr,
            .scl_speed_hz = I2C_MASTER_FREQ_HZ,
        };
        
        i2c_master_dev_handle_t probe_handle;
        esp_err_t ret = i2c_master_bus_add_device(i2c_bus, &probe_cfg, &probe_handle);
        if (ret == ESP_OK) {
            // Try to probe the device with a zero-byte write
            ret = i2c_master_probe(i2c_bus, addr, pdMS_TO_TICKS(10));
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "  Found device at address 0x%02X", addr);
                devices_found++;
            }
            i2c_master_bus_rm_device(probe_handle);
        }
    }
    
    if (devices_found == 0) {
        ESP_LOGW(TAG, "No I2C devices found on bus");
    } else {
        ESP_LOGI(TAG, "Found %d I2C device(s)", devices_found);
    }
    ESP_LOGI(TAG, "PCF8591 expected at address 0x%02X", PCF8591_ADDR);
}

static void usb_monitor_task(void *arg)
{
    bool last_mounted = false;
    bool last_suspended = false;
    
    ESP_LOGI(TAG, "USB Monitor task started");
    ESP_LOGI(TAG, "Waiting for USB host connection...");
    ESP_LOGI(TAG, "TIP: For Android, connect via USB-C cable");
    
    for (;;) {
        bool mounted = tud_mounted();
        bool suspended = tud_suspended();
        
        // Log state changes
        if (mounted != last_mounted) {
            if (mounted) {
                ESP_LOGI(TAG, "=================================");
                ESP_LOGI(TAG, "USB Device CONNECTED!");
                ESP_LOGI(TAG, "Device appears as: USB Audio Output");
                ESP_LOGI(TAG, "Android: Play music/audio on phone");
                ESP_LOGI(TAG, "Audio will stream via UDP to RX");
                ESP_LOGI(TAG, "=================================");
            } else {
                ESP_LOGW(TAG, "USB Device disconnected");
            }
            last_mounted = mounted;
        }
        
        if (suspended != last_suspended) {
            if (suspended) {
                ESP_LOGW(TAG, "USB suspended");
            } else {
                ESP_LOGI(TAG, "USB resumed");
            }
            last_suspended = suspended;
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000)); // Check once per second
    }
}

// TinyUSB descriptor callbacks (required for custom descriptors)
// NOTE: When using espressif__esp_tinyusb component, these callbacks conflict
// with the component's default implementations. We only define them when
// CONFIG_TINYUSB_DESC_CUSTOM is enabled.
#if CONFIG_TINYUSB_DESC_CUSTOM
uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&usb_audio_device_descriptor;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return usb_audio_configuration_descriptor;
}

static uint16_t _desc_str[32];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    uint8_t chr_count;
    
    if (index == 0) {
        _desc_str[0] = (TUSB_DESC_STRING << 8) | (2 + 2);
        _desc_str[1] = 0x0409; // English (United States)
        return _desc_str;
    }
    
    const char *str = (index < sizeof(usb_string_descriptors) / sizeof(usb_string_descriptors[0])) 
                      ? usb_string_descriptors[index] : "";
    
    // Convert ASCII to UTF-16
    chr_count = strlen(str);
    if (chr_count > 31) chr_count = 31;
    
    for (uint8_t i = 0; i < chr_count; i++) {
        _desc_str[1 + i] = str[i];
    }
    
    _desc_str[0] = (TUSB_DESC_STRING << 8) | (2 * chr_count + 2);
    return _desc_str;
}
#endif // CONFIG_TINYUSB_DESC_CUSTOM

static void init_usb_audio(void)
{
    ESP_LOGI(TAG, "╔════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║   Initializing USB Audio Class Device     ║");
    ESP_LOGI(TAG, "╚════════════════════════════════════════════╝");
    
    // Configure TinyUSB with descriptor callbacks
    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = NULL,  // Will use callback
        .string_descriptor = NULL,   // Will use callback
        .string_descriptor_count = 0,
        .external_phy = false,
        .configuration_descriptor = NULL,  // Will use callback
    };
    
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    
    ESP_LOGI(TAG, "✓ USB Audio device configured:");
    ESP_LOGI(TAG, "  • Sample Rate: %d Hz", USB_AUDIO_SAMPLE_RATE);
    ESP_LOGI(TAG, "  • Channels: %d (Mono)", USB_AUDIO_CHANNEL_COUNT);
    ESP_LOGI(TAG, "  • Bit Depth: %d-bit", USB_AUDIO_BIT_RESOLUTION);
    ESP_LOGI(TAG, "  • Device Name: %s", usb_string_descriptors[1]);
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "⚠️  NOTE: Serial monitor will disconnect when USB audio activates!");
    ESP_LOGI(TAG, "   This is expected - USB audio uses the same USB port.");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "📱 For Android testing:");
    ESP_LOGI(TAG, "   1. Connect device to Android phone via USB-C cable");
    ESP_LOGI(TAG, "   2. Phone should show 'USB device detected' notification");
    ESP_LOGI(TAG, "   3. Open any audio recording app (Voice Recorder, etc.)");
    ESP_LOGI(TAG, "   4. Select 'USB Audio Device' as input source");
    ESP_LOGI(TAG, "   5. You should see/hear the audio streaming!");
    ESP_LOGI(TAG, "");
    
    // Start USB monitoring task
    xTaskCreate(usb_monitor_task, "usb_monitor", 4096, NULL, 5, NULL);
}

static void pcf8591_read_task(void *arg)
{
    uint8_t cmd = PCF8591_CHANNEL; // Select AIN0
    int16_t sample_buffer[SAMPLES_PER_PACKET];
    bool pcf8591_available = false;
    i2c_master_dev_handle_t pcf8591_handle = NULL;
    
    // Scan I2C bus to detect devices
    scan_i2c_bus();
    
    // Add I2C device for PCF8591 using existing bus
    i2c_device_config_t pcf8591_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF8591_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    
    esp_err_t ret = i2c_master_bus_add_device(i2c_bus, &pcf8591_cfg, &pcf8591_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add PCF8591 I2C device: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "PCF8591 ADC not available - AUX input will not work");
    } else {
        // Test if device responds
        uint8_t test_read;
        ret = i2c_master_transmit_receive(pcf8591_handle, &cmd, 1, &test_read, 1, pdMS_TO_TICKS(10));
        if (ret == ESP_OK) {
            pcf8591_available = true;
            ESP_LOGI(TAG, "PCF8591 ADC initialized successfully");
        } else {
            ESP_LOGW(TAG, "PCF8591 not responding on I2C bus - check wiring (SDA=GPIO%d, SCL=GPIO%d)", I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO);
        }
    }
    
    int consecutive_errors = 0;
    const int MAX_CONSECUTIVE_ERRORS = 10;
    
    for (;;) {
        // If too many errors, disable device and wait before retrying
        if (consecutive_errors >= MAX_CONSECUTIVE_ERRORS) {
            pcf8591_available = false;
            ESP_LOGW(TAG, "PCF8591 disabled after %d consecutive errors - will retry in 5s", MAX_CONSECUTIVE_ERRORS);
            vTaskDelay(pdMS_TO_TICKS(5000));
            consecutive_errors = 0; // Reset counter to allow retry
            pcf8591_available = true; // Try again
        }
        
        if (audio_mode == AUDIO_INPUT_AUX && audio_ring_buffer != NULL && pcf8591_available) {
            // Read samples at 16kHz rate (one packet every 10ms)
            int errors_in_packet = 0;
            for (int i = 0; i < SAMPLES_PER_PACKET; i++) {
                // Send command and read ADC value using driver_ng API
                uint8_t adc_value = 0;
                esp_err_t ret = i2c_master_transmit_receive(pcf8591_handle, &cmd, 1, &adc_value, 1, pdMS_TO_TICKS(10));
                
                if (ret == ESP_OK) {
                    // Convert 8-bit ADC (0-255) to 16-bit PCM (-32768 to 32767)
                    // Center around 0 and scale up
                    sample_buffer[i] = ((int16_t)adc_value - 128) * 256;
                    consecutive_errors = 0; // Reset on successful read
                } else {
                    sample_buffer[i] = 0; // Silence on error
                    errors_in_packet++;
                }
                
                // Small delay to prevent I2C bus saturation (62.5us per sample at 16kHz)
                vTaskDelay(1); // ~1ms delay, will read slower but safer
            }
            
            // Track consecutive errors across packets
            if (errors_in_packet > SAMPLES_PER_PACKET / 2) {
                consecutive_errors++;
            }
            
            // Write packet to ring buffer
            xRingbufferSend(audio_ring_buffer, sample_buffer, sizeof(sample_buffer), 0);
        } else {
            // Reset error count when not actively using PCF8591
            consecutive_errors = 0;
        }
        
        vTaskDelay(pdMS_TO_TICKS(10)); // 10ms per packet
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
    
    ESP_LOGI(TAG, "UDP socket configured, starting audio streaming...");

    // Tone generation state (for AUDIO_INPUT_TONE mode)
    float phase = 0.0f;
    const float phase_inc = 2.0f * M_PI * TONE_FREQ / SAMPLE_RATE;
    
    int16_t buffer[SAMPLES_PER_PACKET];
    bool has_audio = false;

    while (1) {
        // Route audio based on current mode
        if (audio_mode == AUDIO_INPUT_TONE) {
            // Generate tone - always streaming in tone mode
            for (int i = 0; i < SAMPLES_PER_PACKET; ++i) {
                buffer[i] = (int16_t)(sinf(phase) * 3000.0f);
                phase += phase_inc;
                if (phase > 2.0f * M_PI) phase -= 2.0f * M_PI;
            }
            has_audio = true;
            is_streaming = true;
        } else if (audio_mode == AUDIO_INPUT_USB) {
            // Try to read from TinyUSB audio buffer using direct API
            // Note: TinyUSB audio functions may vary by version
            // For now, fill with silence - USB audio will be handled by callback
            memset(buffer, 0, sizeof(buffer));
            // TODO: Implement proper USB audio buffer reading once API is confirmed
        } else {
            // Read from ring buffer (AUX input)
            size_t item_size = 0;
            int16_t *ring_data = (int16_t *)xRingbufferReceive(audio_ring_buffer, &item_size, pdMS_TO_TICKS(20));
            
            if (ring_data != NULL) {
                // Copy data from ring buffer
                size_t samples_to_copy = (item_size < sizeof(buffer)) ? item_size : sizeof(buffer);
                memcpy(buffer, ring_data, samples_to_copy);
                vRingbufferReturnItem(audio_ring_buffer, ring_data);
                
                // Zero-fill if not enough data
                if (samples_to_copy < sizeof(buffer)) {
                    memset((uint8_t*)buffer + samples_to_copy, 0, sizeof(buffer) - samples_to_copy);
                }
                
                // Check if buffer contains actual audio (not silence)
                // Use a threshold to detect non-zero audio
                const int16_t silence_threshold = 100; // Ignore very quiet noise
                for (int i = 0; i < SAMPLES_PER_PACKET; i++) {
                    if (buffer[i] > silence_threshold || buffer[i] < -silence_threshold) {
                        has_audio = true;
                        break;
                    }
                }
            } else {
                // No data available, fill with silence
                memset(buffer, 0, sizeof(buffer));
                has_audio = false;
            }
        }

        // Update streaming status and send packet if we have actual audio
        is_streaming = has_audio;
        
        if (has_audio) {
            int sent = sendto(sock, (const void *)buffer, sizeof(buffer), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
            if (sent < 0) {
                ESP_LOGW(TAG, "sendto failed: errno=%d", errno);
            } else {
                packet_count++;
                if (display_mode == 0) update_oled_display();
                if (packet_count % 100 == 0) {
                    ESP_LOGI(TAG, "Sent %lu packets (mode: %d)", packet_count, audio_mode);
                }
            }
        } else if (display_mode == 0) {
            // Update display even when not streaming to show "Waiting..."
            static uint32_t last_update = 0;
            if (packet_count - last_update > 10) { // Update every ~100ms
                update_oled_display();
                last_update = packet_count;
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

    // Initialize OLED display (handles I2C bus init with driver_ng)
    vTaskDelay(pdMS_TO_TICKS(100));
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

    // Create audio ring buffer for USB and AUX input
    audio_ring_buffer = xRingbufferCreate(AUDIO_BUFFER_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (audio_ring_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to create audio ring buffer");
    }
    
    // Initialize USB audio (TinyUSB UAC)
    // NOTE: Serial monitor will disconnect when USB audio takes over!
    // Audio data is received via tud_audio_rx_done_post_read_cb()
    init_usb_audio();
    
    // Start PCF8591 ADC reading task for AUX input (only runs in AUX mode)
    xTaskCreate(pcf8591_read_task, "pcf8591_adc", 3072, NULL, 4, NULL);
    
    // Start RX node simulation task
    xTaskCreate(rx_node_sim_task, "rx_node_sim", 2048, NULL, 2, NULL);
    
    start_softap();

    xTaskCreate(udp_sender_task, "udp_sender", 4096, NULL, 5, NULL);
}
