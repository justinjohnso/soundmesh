#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include "config/build.h"
#include "config/pins.h"
#include "control/display.h"
#include "control/buttons.h"
#include "control/status.h"
#include "network/mesh_net.h"
#include "audio/tone_gen.h"
#include "audio/usb_audio.h"
#include "audio/ring_buffer.h"

static const char *TAG = "tx_main";

static tx_status_t status = {
    .input_mode = INPUT_MODE_TONE,
    .audio_active = false,
    .connected_nodes = 0
};

static display_view_t current_view = DISPLAY_VIEW_NETWORK;
static ring_buffer_t *audio_buffer = NULL;

void app_main(void) {
    ESP_LOGI(TAG, "MeshNet Audio TX starting...");
    
    // Initialize control layer
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(buttons_init());
    
    // Initialize network layer (AP mode)
    ESP_ERROR_CHECK(network_init_ap());
    
    // Initialize audio layer
    ESP_ERROR_CHECK(tone_gen_init(440));
    ESP_ERROR_CHECK(usb_audio_init());
    
    // Create ring buffer
    audio_buffer = ring_buffer_create(RING_BUFFER_SIZE);
    if (!audio_buffer) {
        ESP_LOGE(TAG, "Failed to create ring buffer");
        return;
    }
    
    ESP_LOGI(TAG, "TX initialized, starting main loop");
    
    int16_t audio_frame[AUDIO_FRAME_SAMPLES];
    uint8_t packet_buffer[AUDIO_FRAME_BYTES];
    
    while (1) {
        // Handle button events
        button_event_t btn_event = buttons_poll();
        if (btn_event == BUTTON_EVENT_SHORT_PRESS) {
            current_view = (current_view == DISPLAY_VIEW_NETWORK) ? 
                          DISPLAY_VIEW_AUDIO : DISPLAY_VIEW_NETWORK;
            ESP_LOGI(TAG, "View changed to %s", 
                    current_view == DISPLAY_VIEW_NETWORK ? "Network" : "Audio");
        } else if (btn_event == BUTTON_EVENT_LONG_PRESS) {
            status.input_mode = (status.input_mode + 1) % 3;
            ESP_LOGI(TAG, "Input mode changed to %d", status.input_mode);
        }
        
        // Generate/capture audio based on mode
        status.audio_active = false;
        switch (status.input_mode) {
            case INPUT_MODE_TONE:
                tone_gen_fill_buffer(audio_frame, AUDIO_FRAME_SAMPLES);
                status.audio_active = true;
                break;
            case INPUT_MODE_USB:
                if (usb_audio_is_active()) {
                    usb_audio_read_frames(audio_frame, AUDIO_FRAME_SAMPLES);
                    status.audio_active = true;
                }
                break;
            case INPUT_MODE_AUX:
                // TODO: Read from ADC
                break;
        }
        
        // If we have audio, send it
        if (status.audio_active) {
            memcpy(packet_buffer, audio_frame, AUDIO_FRAME_BYTES);
            network_udp_send(packet_buffer, AUDIO_FRAME_BYTES);
        }
        
        // Update display
        display_render_tx(current_view, &status);
        
        vTaskDelay(pdMS_TO_TICKS(AUDIO_FRAME_MS));
    }
}
