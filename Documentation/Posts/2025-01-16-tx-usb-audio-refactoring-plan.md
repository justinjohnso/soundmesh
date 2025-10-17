# TX USB Audio Refactoring Plan

**Date:** January 16, 2025  
**Status:** In Progress - File Needs Reconstruction  
**Issue:** Custom USB descriptors conflict with espressif__esp_tinyusb component

## Problem

The TX firmware currently implements custom USB Audio Class Device descriptors (~300+ lines) that conflict with the `espressif__esp_tinyusb` component's descriptor management, causing linker errors:

```
multiple definition of `tud_descriptor_device_cb'
multiple definition of `tud_descriptor_configuration_cb'  
multiple definition of `tud_descriptor_string_cb'
```

## Solution: Use Espressif's usb_device_uac Component

Based on analysis of [ESP-IOT-Solution USB UAC examples](https://github.com/espressif/esp-iot-solution/tree/main/examples/usb/device/usb_uac), the proper approach is to use Espressif's `usb_device_uac` component which provides:

- Proper UAC2 descriptor management
- Simple callback-based API  
- Full TinyUSB integration
- Speaker/microphone support

## Changes Required

### 1. Update Dependencies

**File:** `firmware/platformio/tx/src/idf_component.yml`

```yaml
dependencies:
  idf:
    version: '>=5.3.0'
  k0i05/esp_ssd1306: '*'
  espressif/usb_device_uac: '^1.0.0'  # CHANGED from esp_tinyusb
```

**File:** `firmware/platformio/tx/platformio.ini`

```ini
build_flags = 
    -D I2C_MASTER_SCL_IO=6
    -D I2C_MASTER_SDA_IO=5
    -D BUTTON_GPIO=4
    # UAC Component Configuration (16kHz mono speaker)
    -D CONFIG_UAC_SAMPLE_RATE=16000
    -D CONFIG_UAC_SPEAKER_CHANNEL_NUM=1
    -D CONFIG_UAC_MIC_CHANNEL_NUM=0
```

### 2. Simplify main.c

**Remove** (lines ~37-50):
- TinyUSB configuration defines (`CFG_TUD_AUDIO_*`)
- Helper macros (`U16_TO_U8S_LE`, `U32_TO_U8S_LE`)
- Direct TinyUSB/tusb.h includes

**Remove** (lines ~397-518):
- Interface/endpoint enums
- Device descriptor struct (`usb_audio_device_descriptor`)
- Configuration descriptor array (`usb_audio_configuration_descriptor`)
- String descriptor array (`usb_string_descriptors`)

**Remove** (lines ~520-595):  
- TinyUSB callback functions:
  - `tud_audio_rx_done_post_read_cb()`
  - `tud_audio_set_itf_cb()`
  - `tud_audio_set_itf_close_EP_cb()`
  - `tud_audio_get_req_entity_cb()`
  - `tud_audio_set_req_entity_cb()`

**Remove** (lines ~671-709):
- Descriptor callback functions:
  - `tud_descriptor_device_cb()`
  - `tud_descriptor_configuration_cb()`
  - `tud_descriptor_string_cb()`

**Replace** init_usb_audio() function with:

```c
#include "usb_device_uac.h"

// UAC callback: Called when audio data arrives FROM the computer
static esp_err_t uac_device_output_cb(uint8_t *buf, size_t len, void *arg)
{
    (void)arg;
    
    // Write incoming USB audio to ring buffer for UDP transmission
    if (audio_ring_buffer != NULL && len > 0 && audio_mode == AUDIO_INPUT_USB) {
        BaseType_t result = xRingbufferSend(audio_ring_buffer, buf, len, pdMS_TO_TICKS(10));
        if (result == pdTRUE) {
            is_streaming = true;
        }
    }
    
    return ESP_OK;
}

static void init_usb_audio(void)
{
    ESP_LOGI(TAG, "╔════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║   Initializing USB Audio Class Device     ║");
    ESP_LOGI(TAG, "╚════════════════════════════════════════════╝");
    
    // Configure UAC device with simple callback
    uac_device_config_t config = {
        .output_cb = uac_device_output_cb,    // Handle incoming audio
        .input_cb = NULL,                      // No microphone input
        .set_mute_cb = NULL,                   // Optional: mute control
        .set_volume_cb = NULL,                 // Optional: volume control
        .cb_ctx = NULL,
    };
    
    esp_err_t ret = uac_device_init(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize UAC device: %s", esp_err_to_name(ret));
        return;
    }
    
    ESP_LOGI(TAG, "✓ USB Audio device initialized successfully");
    ESP_LOGI(TAG, "  • Sample Rate: %d Hz", CONFIG_UAC_SAMPLE_RATE);
    ESP_LOGI(TAG, "  • Channels: %d (Mono)", CONFIG_UAC_SPEAKER_CHANNEL_NUM);
    ESP_LOGI(TAG, "  • Device appears as: USB Audio Speaker");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "📱 Android Usage:");
    ESP_LOGI(TAG, "   1. Connect via USB-C cable");
    ESP_LOGI(TAG, "   2. Play audio - device will route to UDP stream");
    ESP_LOGI(TAG, "");
}
```

## Net Result

- **Removed:** ~350 lines of descriptor/callback code
- **Added:** ~25 lines of simple UAC callback
- **Benefits:**
  - No linker conflicts
  - Proper UAC2 implementation
  - Maintained by Espressif
  - Easier to debug
  - Standard API patterns

## Current Status

### ✅ Completed
- Updated `idf_component.yml` with `usb_device_uac` dependency
- Updated `platformio.ini` with UAC configuration flags
- Component downloads successfully from ESP registry

### ⚠️ Blocked
- `main.c` file is currently in corrupted state from partial edits
- Needs complete reconstruction with clean UAC integration

### 📋 Next Steps
1. Restore/reconstruct clean `main.c` from working backup
2. Apply systematic refactoring following this plan
3. Test build with UAC component
4. Verify USB Audio functionality with Android device
5. Document final working configuration

## References

- [Espressif USB UAC Example](https://github.com/espressif/esp-iot-solution/tree/main/examples/usb/device/usb_uac)
- [USB Device UAC Documentation](https://docs.espressif.com/projects/esp-iot-solution/en/latest/usb/usb_device/usb_device_uac.html)
- [ESP-IOT-Solution usb_device_uac Component](https://components.espressif.com/components/espressif/usb_device_uac)

## Notes

The `usb_device_uac` component internally manages:
- USB PHY initialization  
- TinyUSB stack initialization
- UAC2 descriptors (device, configuration, interface, endpoint)
- Audio streaming interfaces (clock source, terminals, feature units)
- Volume/mute control handlers

All of this is abstracted behind the simple `uac_device_init()` API, making it much more maintainable than custom descriptor implementation.
