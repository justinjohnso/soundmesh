/* MeshNet Audio - RX Node (Modular Architecture)
 * UDP Transport → Depacketizer → Jitter Buffer → I2S Output
 */

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "common/config.h"
#include "common/types.h"
#include "common/packet.h"
#include "network/transport.h"
#include "audio/sink.h"
#include "audio/pipeline.h"
#include "control/ui.h"
#include "control/button.h"

static const char *TAG = "rx_main";

static ui_handle_t ui_handle = NULL;
static button_handle_t button_handle = NULL;
static depacketizer_handle_t depacketizer = NULL;
static jitter_buffer_handle_t jitter_buffer = NULL;
static const transport_vtable_t *transport = &udp_transport;
static const audio_sink_t *sink = &i2s_dac_sink;

static rx_status_t status = {
    .is_streaming = false,
    .packet_count = 0,
    .audio_packet_count = 0,
    .wifi_rssi = -100,
    .mesh_hops = 1,
    .frame_counter = 0,
    .bytes_received = 0,
};

static display_mode_t current_display_mode = DISPLAY_MODE_PRIMARY;

static void audio_rx_task(void *arg) {
    size_t packet_size = packet_total_size(AUDIO_SAMPLES_PER_PACKET);
    audio_packet_t *packet = malloc(packet_size);
    int16_t pcm_buffer[AUDIO_SAMPLES_PER_PACKET];
    uint16_t num_samples;
    
    if (!packet) {
        ESP_LOGE(TAG, "Failed to allocate packet buffer");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Audio RX task started");

    while (1) {
        ssize_t received = transport->recv(packet, packet_size, 100);
        
        if (received > 0) {
            status.bytes_received += received;
            status.packet_count++;
            
            esp_err_t ret = depacketizer_process(depacketizer, packet, pcm_buffer, &num_samples);
            if (ret == ESP_OK) {
                status.audio_packet_count++;
                status.is_streaming = true;
                
                jitter_buffer_push(jitter_buffer, pcm_buffer, num_samples);
            }
            
            status.wifi_rssi = transport->get_rssi();
        } else {
            status.is_streaming = false;
        }
    }
}

static void audio_out_task(void *arg) {
    int16_t audio_buffer[AUDIO_SAMPLES_PER_PACKET];
    
    ESP_LOGI(TAG, "Audio output task started");

    vTaskDelay(pdMS_TO_TICKS(100));

    while (1) {
        esp_err_t ret = jitter_buffer_pop(jitter_buffer, audio_buffer, AUDIO_SAMPLES_PER_PACKET);
        
        if (ret == ESP_OK) {
            sink->write(audio_buffer, AUDIO_SAMPLES_PER_PACKET, 100);
        } else {
            memset(audio_buffer, 0, sizeof(audio_buffer));
            sink->write(audio_buffer, AUDIO_SAMPLES_PER_PACKET, 10);
        }
        
        vTaskDelay(pdMS_TO_TICKS(AUDIO_PACKET_INTERVAL_MS));
    }
}

static void ui_update_task(void *arg) {
    ESP_LOGI(TAG, "UI update task started");

    while (1) {
        status.frame_counter++;
        ui_update_rx(ui_handle, &status, current_display_mode);
        
        if (status.packet_count % 100 == 0 && status.packet_count > 0) {
            status.bytes_received = 0;
        }
        
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void button_handler_task(void *arg) {
    ESP_LOGI(TAG, "Button handler task started");

    while (1) {
        button_event_t event = button_get_event(button_handle, 100);
        
        if (event == BUTTON_EVENT_SHORT_PRESS) {
            current_display_mode = (current_display_mode == DISPLAY_MODE_PRIMARY) ?
                                   DISPLAY_MODE_INFO : DISPLAY_MODE_PRIMARY;
            ESP_LOGI(TAG, "Display mode toggled to %d", current_display_mode);
        }
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "MeshNet Audio RX starting...");

    ESP_ERROR_CHECK(nvs_flash_init());

    i2c_master_bus_handle_t i2c_bus;
    i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_MASTER_NUM,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &i2c_bus));

    ui_config_t ui_cfg = {
        .i2c_bus = i2c_bus,
        .i2c_addr = OLED_I2C_ADDR,
        .is_tx = false,
    };
    ESP_ERROR_CHECK(ui_init(&ui_cfg, &ui_handle));

    button_config_t button_cfg = {
        .gpio_num = BUTTON_GPIO,
        .debounce_ms = BUTTON_DEBOUNCE_MS,
        .long_press_ms = BUTTON_LONG_PRESS_MS,
    };
    ESP_ERROR_CHECK(button_init(&button_cfg, &button_handle));

    transport_config_t transport_cfg = {
        .role = TRANSPORT_ROLE_RX,
        .ssid = MESHNET_SSID,
        .password = MESHNET_PASS,
        .channel = MESHNET_CHANNEL,
        .port = MESHNET_UDP_PORT,
    };
    ESP_ERROR_CHECK(transport->init(&transport_cfg));

    ESP_ERROR_CHECK(depacketizer_init(&depacketizer));

    jitter_buffer_config_t jitter_cfg = {
        .buffer_packets = JITTER_BUFFER_PACKETS,
        .target_latency_ms = JITTER_TARGET_LATENCY_MS,
    };
    ESP_ERROR_CHECK(jitter_buffer_init(&jitter_cfg, &jitter_buffer));

    ESP_ERROR_CHECK(sink->init(NULL));

    xTaskCreate(audio_rx_task, "audio_rx", 4096, NULL, 5, NULL);
    xTaskCreate(audio_out_task, "audio_out", 3072, NULL, 5, NULL);
    xTaskCreate(ui_update_task, "ui_update", 3072, NULL, 3, NULL);
    xTaskCreate(button_handler_task, "button", 2048, NULL, 4, NULL);

    ESP_LOGI(TAG, "MeshNet Audio RX initialized successfully");
}
