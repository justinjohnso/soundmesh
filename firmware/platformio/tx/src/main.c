/* MeshNet Audio - TX Node (Modular Architecture)
 * Audio Input → Packetizer → UDP Transport
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
#include "audio/source.h"
#include "audio/pipeline.h"
#include "control/ui.h"
#include "control/button.h"

static const char *TAG = "tx_main";

static ui_handle_t ui_handle = NULL;
static button_handle_t button_handle = NULL;
static packetizer_handle_t packetizer = NULL;
static const transport_vtable_t *transport = &udp_transport;
static const audio_source_t *current_source = &tone_source;

static tx_status_t status = {
    .audio_mode = AUDIO_INPUT_TONE,
    .is_streaming = false,
    .packet_count = 0,
    .rx_node_count = 1,
    .frame_counter = 0,
};

static display_mode_t current_display_mode = DISPLAY_MODE_PRIMARY;

static void audio_tx_task(void *arg) {
    int16_t audio_buffer[AUDIO_SAMPLES_PER_PACKET];
    size_t packet_size = packet_total_size(AUDIO_SAMPLES_PER_PACKET);
    audio_packet_t *packet = malloc(packet_size);
    
    if (!packet) {
        ESP_LOGE(TAG, "Failed to allocate packet buffer");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Audio TX task started");

    while (1) {
        size_t frames_read = current_source->read(audio_buffer, AUDIO_SAMPLES_PER_PACKET, 100);
        
        if (frames_read > 0) {
            esp_err_t ret = packetizer_process(packetizer, audio_buffer, frames_read, packet);
            if (ret == ESP_OK) {
                ssize_t sent = transport->send(packet, packet_size);
                if (sent > 0) {
                    status.packet_count++;
                    status.is_streaming = true;
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(AUDIO_PACKET_INTERVAL_MS));
    }
}

static void ui_update_task(void *arg) {
    ESP_LOGI(TAG, "UI update task started");

    while (1) {
        status.frame_counter++;
        ui_update_tx(ui_handle, &status, current_display_mode);
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
        else if (event == BUTTON_EVENT_LONG_PRESS) {
            status.audio_mode = (audio_input_mode_t)((status.audio_mode + 1) % 3);
            
            current_source->deinit();
            
            switch (status.audio_mode) {
                case AUDIO_INPUT_TONE:
                    current_source = &tone_source;
                    break;
                case AUDIO_INPUT_USB:
                    current_source = &usb_source;
                    break;
                case AUDIO_INPUT_AUX:
                    current_source = &aux_source;
                    break;
            }
            
            current_source->init(NULL);
            ESP_LOGI(TAG, "Audio mode changed to %d", status.audio_mode);
        }
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "MeshNet Audio TX starting...");

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
        .is_tx = true,
    };
    ESP_ERROR_CHECK(ui_init(&ui_cfg, &ui_handle));

    button_config_t button_cfg = {
        .gpio_num = BUTTON_GPIO,
        .debounce_ms = BUTTON_DEBOUNCE_MS,
        .long_press_ms = BUTTON_LONG_PRESS_MS,
    };
    ESP_ERROR_CHECK(button_init(&button_cfg, &button_handle));

    transport_config_t transport_cfg = {
        .role = TRANSPORT_ROLE_TX,
        .ssid = MESHNET_SSID,
        .password = MESHNET_PASS,
        .channel = MESHNET_CHANNEL,
        .port = MESHNET_UDP_PORT,
    };
    ESP_ERROR_CHECK(transport->init(&transport_cfg));

    ESP_ERROR_CHECK(current_source->init(NULL));

    packetizer_config_t packetizer_cfg = {
        .samples_per_packet = AUDIO_SAMPLES_PER_PACKET,
    };
    ESP_ERROR_CHECK(packetizer_init(&packetizer_cfg, &packetizer));

    xTaskCreate(audio_tx_task, "audio_tx", 4096, NULL, 5, NULL);
    xTaskCreate(ui_update_task, "ui_update", 3072, NULL, 3, NULL);
    xTaskCreate(button_handler_task, "button", 2048, NULL, 4, NULL);

    ESP_LOGI(TAG, "MeshNet Audio TX initialized successfully");
}
