#include "audio/usb_audio.h"
#include "config/build.h"
#include <esp_log.h>
#include <string.h>
#include <math.h>

#ifdef CONFIG_TX_BUILD
#include <tusb.h>
#include "audio/ring_buffer.h"
#include "esp_private/usb_phy.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

static const char *TAG = "usb_audio";

#ifdef CONFIG_TX_BUILD
// USB audio speaker implementation
#define USB_AUDIO_BUFFER_SIZE (AUDIO_FRAME_SAMPLES * 4 * 4)  // Buffer for 4 frames stereo (bytes)
static ring_buffer_t *usb_audio_buffer = NULL;
static bool usb_initialized = false;
static bool usb_audio_active = false;

// Audio format: 44.1kHz, 16-bit, stereo
#define USB_SAMPLE_RATE 44100
#define USB_CHANNELS 2
#define USB_BITS_PER_SAMPLE 16

// TinyUSB audio callbacks
void tud_audio_rx_done_pre_read_cb(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out, uint8_t cur_alt_setting) {
    (void)rhport;
    (void)func_id;
    (void)ep_out;
    (void)cur_alt_setting;

    if (!usb_audio_buffer) {
        return;
    }

    uint8_t buf[CFG_TUD_AUDIO_EP_SZ_OUT];
    uint16_t n_bytes_read = tud_audio_read(buf, MIN(n_bytes_received, sizeof(buf)));

    // Write audio data to ring buffer
    if (ring_buffer_write(usb_audio_buffer, buf, n_bytes_read) != ESP_OK) {
        ESP_LOGW(TAG, "USB audio buffer full, dropping %d bytes", n_bytes_read);
    }

    usb_audio_active = true;
}

bool tud_audio_set_req_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *pBuff, uint16_t *pLen) {
    (void)rhport;
    (void)pBuff;
    (void)pLen;

    // Handle set interface request
    if (p_request->bRequest == AUDIO_CS_REQ_CUR && p_request->wValue == AUDIO_CS_AS_INTERFACE_CONTROL_SAM_FREQ) {
        // Sample rate set, but we ignore for now
        return true;
    }
    return false;
}

bool tud_audio_get_req_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *pBuff, uint16_t *pLen) {
    (void)rhport;
    (void)pBuff;
    (void)pLen;

    // Handle get interface request
    if (p_request->bRequest == AUDIO_CS_REQ_CUR && p_request->wValue == AUDIO_CS_AS_INTERFACE_CONTROL_SAM_FREQ) {
        // Return current sample rate
        uint32_t sample_rate = USB_SAMPLE_RATE;
        memcpy(pBuff, &sample_rate, sizeof(sample_rate));
        *pLen = sizeof(sample_rate);
        return true;
    }
    return false;
}
#endif

// USB task for TinyUSB
static void usb_task(void *pvParam) {
    (void)pvParam;
    while (1) {
        tud_task();
        vTaskDelay(1);  // Small delay to prevent watchdog
    }
}

esp_err_t usb_audio_init(void) {
#ifdef CONFIG_TX_BUILD
ESP_LOGI(TAG, "USB audio speaker init using TinyUSB");

// Initialize USB PHY for internal USB
usb_phy_handle_t phy_hdl;
usb_phy_config_t phy_conf = {
.controller = USB_PHY_CTRL_OTG,
    .otg_mode = USB_OTG_MODE_DEVICE,
        .target = USB_TARGET_INTERNAL,
    };
ESP_ERROR_CHECK(usb_new_phy(&phy_conf, &phy_hdl));

// Create ring buffer for USB audio data
usb_audio_buffer = ring_buffer_create(USB_AUDIO_BUFFER_SIZE);
if (!usb_audio_buffer) {
        ESP_LOGE(TAG, "Failed to create USB audio buffer");
    return ESP_FAIL;
}

// Initialize TinyUSB
tusb_init();

// Create USB task
xTaskCreate(usb_task, "usb_task", 4096, NULL, 5, NULL);

usb_initialized = true;
    ESP_LOGI(TAG, "TinyUSB audio device initialized - device should appear as audio output on host");
ESP_LOGI(TAG, "If not visible, try disconnecting and reconnecting USB cable");
return ESP_OK;
#else
    ESP_LOGI(TAG, "USB audio not supported on RX");
    return ESP_OK;
#endif
}

esp_err_t usb_audio_read_frames(int16_t *buffer, size_t num_samples) {
#ifdef CONFIG_TX_BUILD
if (!usb_audio_active || !usb_audio_buffer) {
memset(buffer, 0, num_samples * sizeof(int16_t));
usb_audio_active = false;  // Reset active flag if no buffer
    return ESP_OK;
    }

// Read from USB audio buffer
size_t bytes_to_read = num_samples * sizeof(int16_t);
if (ring_buffer_read(usb_audio_buffer, (uint8_t*)buffer, bytes_to_read) != ESP_OK) {
// Not enough data, fill with silence and mark inactive
    memset(buffer, 0, bytes_to_read);
        usb_audio_active = false;
}

return ESP_OK;
#else
// RX doesn't support USB audio
    memset(buffer, 0, num_samples * sizeof(int16_t));
    return ESP_OK;
#endif
}

bool usb_audio_is_active(void) {
#ifdef CONFIG_TX_BUILD
    return usb_initialized || usb_audio_active;
#else
    return false;
#endif
}
