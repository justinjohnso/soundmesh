// Button (mode toggle)
#define BUTTON_GPIO 4
#define BUTTON_DEBOUNCE_MS 50
static volatile int display_mode = 0;

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

        #include "ssd1306.h"
        static ssd1306_t dev;

        static void init_oled() {
            ESP_LOGI(TAG, "Initializing OLED display (k0i05/esp_ssd1306)...");
            ssd1306_init(&dev, 128, 32); // Adjust width/height if needed
            ssd1306_clear(&dev);
            ssd1306_draw_string(&dev, 0, 0, "RX Ready", 12, true);
            ssd1306_refresh(&dev);
        }

        static void update_oled_display() {
            ssd1306_clear(&dev);
            char buf[32];
            if (display_mode == 0) {
                // View 0: Mesh stats (latency/hops)
                snprintf(buf, sizeof(buf), "Latency: %d ms", mesh_latency_ms);
                ssd1306_draw_string(&dev, 0, 0, buf, 12, true);
                snprintf(buf, sizeof(buf), "Hops: %d", mesh_hops);
                ssd1306_draw_string(&dev, 0, 16, buf, 12, true);
            } else {
                // View 1: Audio streaming waveform animation
                ssd1306_draw_string(&dev, 0, 0, "Streaming...", 12, true);
                for (int x = 0; x < 128; x++) {
                    float phase = ((float)x / 128.0f) * 2.0f * M_PI + (float)(packet_count % 100) * 0.1f;
                    int y = 16 + (int)(sin(phase) * 10.0f);
                    if (y >= 0 && y < 32) {
                        ssd1306_draw_pixel(&dev, x, y);
                    }
                }
            }
            ssd1306_refresh(&dev);
        }
                // View 0: Mesh latency and hops (simulated)
                snprintf(buf, sizeof(buf), "Latency: %d ms\nHops: %d", mesh_latency_ms, mesh_hops);
                ssd1306_display_text(&dev, 0, buf, strlen(buf), false);
            } else {
                // View 1: Audio streaming waveform animation
                ssd1306_display_text(&dev, 0, "Streaming...", 11, false);
                uint8_t wave[128];
                int y_base = 2; // page 2 (middle)
                for (int x = 0; x < 128; x++) {
                    float phase = ((float)x / 128.0f) * 2.0f * M_PI + (float)(packet_count % 100) * 0.1f;
                    int y = y_base * 8 + 4 + (int)(sin(phase) * 10.0f);
                    if (y >= 0 && y < 32) {
                        wave[x] = 1 << (y % 8);
                    } else {
                        wave[x] = 0;
                    }
                }
                ssd1306_display_image(&dev, y_base, 0, wave, 128);
            }
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

            #include "Adafruit_SSD1306.h"

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
                display.println("RX Ready");
                display.display();
            }

            static void update_oled_display() {
                display.clearDisplay();
                if (display_mode == 0) {
                    // View 0: Mesh latency and hops (simulated)
                    display.setCursor(0,0);
                    display.print("Latency: ");
                    display.print(mesh_latency_ms);
                    display.print(" ms\nHops: ");
                    display.println(mesh_hops);
                } else {
                    // View 1: Audio streaming waveform animation
                    display.setCursor(0,0);
                    display.println("Streaming...");
                    int y_base = 16;
                    for (int x = 0; x < 128; x++) {
                        float phase = ((float)x / 128.0f) * 2.0f * M_PI + (float)(packet_count % 100) * 0.1f;
                        int y = y_base + (int)(sin(phase) * 10.0f);
                        display.drawPixel(x, y, SSD1306_WHITE);
                    }
                }
                display.display();
            }
// Simulate mesh latency and hops by incrementing every 5 seconds
static void mesh_stats_sim_task(void *arg) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        mesh_latency_ms += 10;
        if (mesh_latency_ms > 100) mesh_latency_ms = 10;
        mesh_hops++;
        if (mesh_hops > 4) mesh_hops = 1;
        if (display_mode == 0) update_oled_display();
    }
}
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Started STA and attempting to connect to '%s'", RX_SSID);
}

// Output audio via I2S (UDA1334 DAC)
static void output_audio_i2s(const int16_t *samples, int n) {
    size_t bytes_written = 0;
    i2s_write(I2S_NUM, (const char *)samples, n * sizeof(int16_t), &bytes_written, 10);
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
        
        // Output audio via I2S (if we received 320 bytes = 160 samples)
        if (len == 320) {
            output_audio_i2s((int16_t*)rx_buf, 160);
        }
        
        // Update display every 10 packets (~100ms)
        if (packet_count % 10 == 0) {
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
    
    // Initialize I2S for UDA1334 DAC
    i2s_config_t i2s_cfg = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX,
        .sample_rate = 16000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_I2S_MSB,
        .intr_alloc_flags = 0,
        .dma_buf_count = 4,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };
    i2s_pin_config_t pin_cfg = {
        .bck_io_num = I2S_BCK_IO,
        .ws_io_num = I2S_WS_IO,
        .data_out_num = I2S_DO_IO,
        .data_in_num = I2S_DI_IO
    };
    ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM, &i2s_cfg, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM, &pin_cfg));
    ESP_ERROR_CHECK(i2s_set_clk(I2S_NUM, 16000, I2S_BITS_PER_SAMPLE_16BIT, 2));
    
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

    // Start mesh stats simulation task
    xTaskCreate(mesh_stats_sim_task, "mesh_stats_sim", 1024, NULL, 2, NULL);

    start_sta();

    xTaskCreate(udp_receive_task, "udp_rx", 8192, NULL, 5, NULL);
}
