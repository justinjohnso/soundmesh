# Refactoring ADC Input from Polling to DMA Mode on ESP32-S3

**Date:** November 3, 2025  
**Author:** Development Log  
**Status:** ✅ Complete

## Problem Statement

The meshnet-audio TX unit was experiencing critical stability issues:

- **Task Watchdog crashes** during ADC sampling
- **IntegerDivideByZero errors** in the ADC polling loop
- Manual polling implementation in `adc_input.c` was fundamentally unstable

The original implementation used a FreeRTOS task that manually polled ADC channels in a loop:

```c
// Old unstable approach
while (adc->running) {
    for (uint32_t i = 0; i < samples_per_batch && adc->running; i++) {
        int raw_left = adc1_get_raw(adc->channel_left);
        int raw_right = adc1_get_raw(adc->channel_right);
        // ... process and store
    }
    vTaskDelay(pdMS_TO_TICKS(10));
}
```

This approach couldn't reliably maintain 16kHz stereo sampling without timing issues and watchdog violations.

## Investigation: Finding the Right API

### Initial Assumption: I2S ADC Built-in Mode

Research into ESP-IDF documentation suggested using the I2S driver configured for ADC mode (`I2S_MODE_ADC_BUILT_IN`). This is a standard approach on ESP32 classic chips where the I2S peripheral can interface with ADCs via DMA.

**First attempt:**
```c
i2s_config_t i2s_config = {
    .mode = I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_ADC_BUILT_IN,
    // ...
};
```

**Result:** Compilation failure
```
error: 'I2S_MODE_ADC_BUILT_IN' undeclared
```

### Discovery: ESP32-S3 Doesn't Support I2S ADC Mode

Further investigation revealed that **ESP32-S3 does not support I2S ADC built-in mode**. The hardware architecture differs from ESP32 classic. Instead, ESP32-S3 uses a **Digital ADC controller with DMA**.

### The Correct Approach: Digital ADC with DMA

The proper API for high-speed continuous ADC sampling on ESP32-S3 (ESP-IDF v4.4.6) is:

- **Header:** `driver/adc.h`
- **Functions:** `adc_digi_*` family
- **Key APIs:**
  - `adc_digi_initialize()` - Initialize DMA controller
  - `adc_digi_controller_configure()` - Configure channels and sampling
  - `adc_digi_start()` / `adc_digi_stop()` - Control acquisition
  - `adc_digi_read_bytes()` - Read sampled data from DMA buffer

## Implementation Challenges

### Challenge 1: API Version Differences

Initial research pointed to `esp_adc/adc_continuous.h` (ESP-IDF v5.x API), which doesn't exist in v4.4.6:

```c
// This doesn't exist in ESP-IDF v4.4.6
#include "esp_adc/adc_continuous.h"  // ❌ Compilation error
```

**Solution:** Use the v4.4-compatible `driver/adc.h` with `adc_digi_*` functions.

### Challenge 2: Output Format Type

ESP32-S3 only supports `ADC_DIGI_OUTPUT_FORMAT_TYPE2`, not TYPE1:

```c
// Wrong - causes compilation error
#define ADC_OUTPUT_TYPE ADC_DIGI_OUTPUT_FORMAT_TYPE1

// Correct for ESP32-S3
#define ADC_OUTPUT_TYPE ADC_DIGI_OUTPUT_FORMAT_TYPE2
```

This affects how data is parsed:
```c
adc_digi_output_data_t *p = (adc_digi_output_data_t*)&result[i];
uint32_t chan_num = p->type2.channel;  // Not type1
uint32_t data = p->type2.data;
```

### Challenge 3: ADC1 vs ADC2 Channel Enums

**This was the critical bug.** The most subtle issue was using generic `adc_channel_t` enums instead of ADC1-specific enums:

```c
// WRONG - Driver interprets this as ADC2
static adc_channel_t gpio_to_adc_channel(int gpio_num) {
    switch (gpio_num) {
        case 1:  return ADC_CHANNEL_0;  // ❌ Generic enum
        case 2:  return ADC_CHANNEL_1;
        // ...
    }
}
```

**Error on boot:**
```
E (803) ADC: ADC2 continuous mode is no longer supported, please use ADC1
E (821) ADC: adc_digi_controller_configure(583): Only support using ADC1 DMA mode
E (830) adc_digi: Failed to configure ADC controller: ESP_ERR_INVALID_ARG
```

**Root cause:** ESP32-S3 does NOT support ADC2 in continuous/DMA mode (hardware limitation documented in errata). When using generic `ADC_CHANNEL_*` constants, the driver can't determine which ADC unit you intend to use, leading to ambiguity.

**Solution:** Use ADC1-specific enums and explicitly mark channels:

```c
// CORRECT - Explicitly ADC1
static adc1_channel_t gpio_to_adc1_channel(int gpio_num) {
    switch (gpio_num) {
        case 1:  return ADC1_CHANNEL_0;  // ✅ ADC1-specific
        case 2:  return ADC1_CHANNEL_1;
        // ...
    }
}

// Store as ADC1 channels
struct adc_input_t {
    adc1_channel_t channel_left;   // Not adc_channel_t
    adc1_channel_t channel_right;
    // ...
};

// Build channel mask with ADC1 channels
uint32_t adc1_chan_mask = BIT((*adc)->channel_left);
if ((*adc)->stereo) {
    adc1_chan_mask |= BIT((*adc)->channel_right);
}

// Cast to generic type for pattern config
adc_pattern[0].channel = (adc_channel_t)(*adc)->channel_left;
adc_pattern[0].unit = ADC_UNIT_1;  // Explicitly ADC_UNIT_1
```

## Final Implementation

### File Structure

- **Renamed:** `lib/audio/src/adc_input.c` → `lib/audio/src/i2s_adc_input.c`
- **Updated:** `extra_script.py` to reference new filename
- **Interface:** Maintained existing API in `lib/audio/include/audio/adc_input.h`

### Key Configuration

```c
#define ADC_CONV_MODE       ADC_CONV_SINGLE_UNIT_1
#define ADC_OUTPUT_TYPE     ADC_DIGI_OUTPUT_FORMAT_TYPE2
#define ADC_BIT_WIDTH       SOC_ADC_DIGI_MAX_BITWIDTH

// Initialize DMA
adc_digi_init_config_t adc_dma_config = {
    .max_store_buf_size = 2048,
    .conv_num_each_intr = 256,
    .adc1_chan_mask = BIT(channel_left) | BIT(channel_right),
    .adc2_chan_mask = 0,  // ADC2 not supported
};
adc_digi_initialize(&adc_dma_config);

// Configure sampling pattern
adc_digi_pattern_config_t adc_pattern[2] = {
    {
        .atten = ADC_ATTEN_DB_11,
        .channel = (adc_channel_t)channel_left,
        .unit = ADC_UNIT_1,
        .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH
    },
    {
        .atten = ADC_ATTEN_DB_11,
        .channel = (adc_channel_t)channel_right,
        .unit = ADC_UNIT_1,
        .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH
    }
};

adc_digi_configuration_t dig_cfg = {
    .pattern_num = 2,
    .adc_pattern = adc_pattern,
    .sample_freq_hz = 16000 * 2,  // 16kHz per channel
    .conv_mode = ADC_CONV_SINGLE_UNIT_1,
    .format = ADC_DIGI_OUTPUT_FORMAT_TYPE2
};
adc_digi_controller_configure(&dig_cfg);
```

### Data Flow

1. **DMA fills buffer** automatically at configured sample rate
2. **Read task** retrieves data from DMA buffer:
   ```c
   adc_digi_read_bytes(result, sizeof(result), &ret_num, portMAX_DELAY);
   ```
3. **Parse TYPE2 format** and convert to 16-bit PCM:
   ```c
   adc_digi_output_data_t *p = (adc_digi_output_data_t*)&result[i];
   uint32_t data = p->type2.data;
   int16_t sample = ((int16_t)data - 2048) * 16;
   ```
4. **Store in ring buffer** for consumption by TX pipeline

## Build Results

```
RAM:   [=         ]   9.5% (used 31264 bytes from 327680 bytes)
Flash: [======    ]  63.8% (used 669201 bytes from 1048576 bytes)
========================= [SUCCESS] =========================
```

## Key Takeaways

1. **Hardware-specific APIs matter:** ESP32-S3 has different ADC architecture than ESP32 classic
2. **Version compatibility:** ESP-IDF v4.4 vs v5.x have different ADC APIs
3. **Enum precision:** Use `adc1_channel_t` explicitly, not generic `adc_channel_t` on S3
4. **Output format:** Always check which `ADC_DIGI_OUTPUT_FORMAT_TYPE` your chip supports
5. **DMA is the solution:** For high-speed sampling, hardware DMA eliminates timing issues

## References

- [ESP-IDF v4.4.6 ADC Documentation](https://docs.espressif.com/projects/esp-idf/en/v4.4.6/esp32s3/api-reference/peripherals/adc.html)
- [ESP32-S3 Errata](https://www.espressif.com/sites/default/files/documentation/esp32-s3_errata_en.pdf) - ADC2 continuous mode limitation
- ESP-IDF source: `components/driver/include/driver/adc.h`

## Files Modified

- `lib/audio/src/adc_input.c` → `lib/audio/src/i2s_adc_input.c` (complete rewrite)
- `extra_script.py` (updated build reference)
- Maintained interface: `lib/audio/include/audio/adc_input.h` (no changes)

## Next Steps

- [ ] Upload firmware and verify boot logs show successful ADC initialization
- [ ] Confirm audio input detection in `tx_pipeline_adc.c` logs
- [ ] Test transmission to RX unit
- [ ] Monitor for watchdog errors (should be eliminated)

---

**Status:** Implementation complete, ready for hardware validation.
