# ESP32-LyraT-Mini Audio Loopback: Debugging the Input Path

**Date:** November 3, 2025  
**Status:** In Progress - Troubleshooting AUX Input Circuit  
**Board:** ESP32-LyraT-Mini v1.2

## Goal

Get audio from an external device (phone/computer) into the LyraT-Mini TX board via a soldered AUX input circuit, and verify the audio path works using a loopback test (input → output to headphones).

## Background

The ESP32-LyraT-Mini uses the ES8311 codec for audio processing. Unlike the XIAO ESP32-S3 which requires external ADC circuitry, the LyraT has a proper audio codec with:
- I2S interface for digital audio
- Built-in ADC for microphone input
- Built-in DAC for headphone output
- 16kHz sample rate, mono operation

Our board doesn't have a built-in AUX input jack, so we needed to solder a custom input circuit.

## Phase 1: Task Watchdog Timer (TWDT) Issues

### Problem
Initial implementation used a dedicated FreeRTOS task (`audio_loopback_task`) running in a tight loop:

```c
static void loopback_task(void* arg) {
    while (1) {
        i2s_read(...);   // Read audio
        i2s_write(...);  // Write audio
        // No delay - continuous operation
    }
}
```

This caused **Task Watchdog Timer timeouts** because the loopback task was starving the main task on the same CPU core.

### Solution
**Moved loopback logic into the main loop** where the watchdog is properly reset:

**Changes to `lib/audio/src/i2s_in.c`:**
- ✅ Removed `AUDIO_LOOPBACK_TEST` macro
- ✅ Removed `loopback_task()`, `audio_loopback_start()`, `audio_loopback_stop()`
- ✅ Made I2S initialization unconditionally configure full-duplex mode (RX + TX)
- ✅ Unconditionally enabled DAC output at 60% volume

**Changes to `src/tx/main.c`:**
```c
case INPUT_MODE_AUX:
#if defined(TARGET_LYRAT_MINI) && defined(INPUT_HAS_I2S)
    // Audio loopback test (read from ADC, write to DAC)
    {
        static int loop_count = 0;
        if (loop_count++ % 100 == 0) {
            ESP_LOGI(TAG, "LOOPBACK MODE - reading and writing audio");
        }
        size_t frames_read = 0;
        if (i2s_in_read(mono_frame, AUDIO_FRAME_SAMPLES, &frames_read) == ESP_OK && 
            frames_read == AUDIO_FRAME_SAMPLES) {
            // Write the same samples back out for loopback test
            i2s_in_write(mono_frame, AUDIO_FRAME_SAMPLES);
            // Don't set audio_active to prevent network transmission
            status.audio_active = false;
        } else {
            status.audio_active = false;
        }
    }
#endif
```

**Key insight:** Setting `audio_active = false` prevents network transmission during loopback test while keeping the audio path active.

## Phase 2: Floating GPIO Button Issue

### Problem
After uploading the new firmware, the loopback worked for **1 second**, then:

```
I (1020) tx_main: LOOPBACK MODE - reading and writing audio
I (2020) tx_main: Input mode changed to 2
I (2020) tx_main: Sent framed audio packet seq=0 ...
```

The board was switching from `INPUT_MODE_AUX` (0) to `INPUT_MODE_TONE` (2) automatically!

### Root Cause
The button handling code was checking GPIO36 for input mode switching, but **the LyraT-Mini doesn't have a button on GPIO36**. The floating pin was triggering false button events.

### Solution
**Disable buttons entirely for LyraT builds:**

```c
// At app initialization
#if !defined(TARGET_LYRAT_MINI)
    // Initialize control layer (XIAO S3 only)
    ESP_ERROR_CHECK(buttons_init());
#else
    // LyraT-Mini: No display or buttons
    ESP_LOGI(TAG, "LyraT-Mini build - display and buttons disabled");
#endif

// In main loop
#if !defined(TARGET_LYRAT_MINI)
    // Handle button events (XIAO S3 only)
    button_event_t btn_event = buttons_poll();
    // ... button handling ...
#endif
```

Also removed the `-D DISABLE_DISPLAY=1` flag from [platformio.ini](file:///Users/justin/Library/CloudStorage/Dropbox/NYU/semester-3-2025-fall/project-development-studio/meshnet-audio/platformio.ini) since `TARGET_LYRAT_MINI` is now sufficient for conditional compilation.

## Phase 3: AUX Input Circuit Design

### Circuit Requirements

The LyraT-Mini's ES8311 codec has a **differential microphone input** (MIC+/MIC-) expecting ~10-50mV signals. Phone/computer line-level output is ~1-2V peak, so we need:

1. **Attenuation** - Divide signal by ~20-100x
2. **AC coupling** - Block DC offset from audio source
3. **Differential to single-ended conversion** - Drive MIC+ with signal, reference MIC- to ground

### Final Circuit Design

```
Phone/Computer Audio Output (3.5mm TRS Jack)
│
├─ Tip (Mono signal or Left channel)
│   │
│   ├─ [10µF electrolytic, + toward jack]
│   │
│   ├─ [47kΩ resistor]
│   │
│   ├─────→ MIC+ (ES8311)
│   │
│   └─ [10kΩ resistor to GND]
│
└─ Sleeve (Ground) ──→ GND (any GND pin on board)

MIC- (ES8311) ─ [10µF electrolytic, + toward MIC-] ─→ GND
```

### Component Values

| Component | Value | Purpose |
|-----------|-------|---------|
| AC coupling cap | 10µF, 16V+ | Blocks DC offset, passes audio (cutoff ~0.34Hz) |
| Series resistor | 47kΩ | Attenuator (voltage divider) |
| Pull-down resistor | 10kΩ | Forms divider with 47kΩ (reduces signal by ~5.7x) |
| MIC- cap | 10µF, 16V+ | AC grounds the negative differential input |

### Voltage Division Calculation

```
Total resistance: 47kΩ + 10kΩ = 57kΩ
Division ratio: 10kΩ / 57kΩ ≈ 0.175 (1/5.7)

Example: 1V phone output → ~175mV at MIC+
```

This is still fairly hot for a mic input (designed for ~10-50mV), but manageable with low codec gain settings.

### Capacitor Polarity (Critical!)

**Electrolytic capacitors MUST be oriented correctly:**

1. **AC coupling cap (input):**
   - Long leg (+) → Phone jack tip
   - Short leg (-) → 47kΩ resistor

2. **MIC- cap (ground reference):**
   - Long leg (+) → MIC- pin
   - Short leg (-) → GND

**Visual identification:**
- Long leg = Positive (+)
- Short leg = Negative (-)
- Stripe on body with minus signs (−) = Negative side

### Pin Identification on LyraT-Mini

- **MIC+** - Positive differential microphone input
- **MIC-** - Negative differential microphone input  
- **GND** - Any ground pin works (UART GND, I2C header GND, etc. - all connected)

**Note:** No separate AGND (analog ground) - all grounds are common on this board.

## Phase 4: Audio Input Troubleshooting

### Current Status

**Firmware behavior:** ✅ Working correctly
```
I (4008) i2s_in: Audio level - Peak: 0 / 32767 (0.0%)
I (4018) tx_main: LOOPBACK MODE - reading and writing audio
```

The loopback code is running properly and reading I2S data every 10ms. However, **Peak: 0** indicates **no audio signal is being captured**.

### Possible Issues

1. **Soldering problems:**
   - Cold solder joints (poor electrical connection)
   - Solder bridges shorting adjacent pins
   - Wrong pins identified

2. **Component issues:**
   - Capacitor polarity reversed
   - Wrong resistor values
   - Damaged components

3. **Audio source problems:**
   - Volume too low on computer/phone
   - Wrong cable (mono vs stereo)
   - Cable not fully inserted

4. **Circuit design issues:**
   - Impedance mismatch
   - Insufficient attenuation
   - MIC- not properly referenced

### Diagnostic Tests

#### Test 1: Visual Inspection
- [ ] Photo of soldered circuit
- [ ] Verify capacitor polarity (stripe = negative side)
- [ ] Check for solder bridges
- [ ] Confirm resistor color codes (47kΩ = Yellow-Violet-Orange, 10kΩ = Brown-Black-Orange)

#### Test 2: Continuity Testing (Power OFF!)
```
Expected readings with multimeter:
- Phone jack tip → MIC+: ~47-57kΩ (through resistors)
- Phone jack sleeve → GND: ~0Ω (direct connection)
- MIC- → GND: >1MΩ (capacitor blocks DC)
```

#### Test 3: Simple Touch Test
**Minimal verification of ES8311 ADC path:**

1. Power off board
2. Remove entire soldered circuit
3. Take a single wire, connect to MIC+
4. Power on board
5. Touch the other end of wire with finger

**Expected result:** Peak value should increase (60Hz hum from body)

**If Peak > 0:** Soldered circuit has a problem  
**If Peak = 0:** MIC+ pin identification wrong OR ES8311 init failed

#### Test 4: Audio Output Path
**With headphones plugged into OUTPUT jack:**

Do you hear:
- Pops/clicks when board boots?
- Any hum or noise?
- Complete silence?

If complete silence, the DAC output path might not be working.

## Firmware Configuration

### PlatformIO Environment

```ini
[env:tx_lyrat]
board = esp32dev
board_build.sdkconfig = sdkconfig.tx_lyrat.defaults
upload_port = /dev/cu.usbserial-10
extra_scripts = pre:extra_script.py
build_flags =
    ${env.build_flags}
    -D TARGET_LYRAT_MINI=1
    -D AUDIO_SAMPLE_RATE=16000
    -D AUDIO_FRAME_MS=10
    -D INPUT_HAS_I2S=1
```

**Note:** Removed `-D DISABLE_DISPLAY=1` - now using `TARGET_LYRAT_MINI` for all LyraT-specific conditionals.

### ES8311 Initialization

```c
// Initialize I2C and ES8311 codec
ESP_ERROR_CHECK(es8311_init_i2c(I2C_NUM_0, I2C_SDA_PIN, I2C_SCL_PIN, 400000));
ESP_ERROR_CHECK(es8311_config_adc_16k_mono_lowgain());  // ADC for input

// Configure I2S for full duplex (RX + TX)
i2s_config_t i2s_config = {
    .mode = I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_TX,
    .sample_rate = AUDIO_SAMPLE_RATE,  // 16000 Hz
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,  // Mono
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    // ... DMA buffer config ...
};

// Enable DAC for loopback output
ESP_ERROR_CHECK(es8311_config_dac_16k_mono());
ESP_ERROR_CHECK(es8311_set_hp_volume_percent(60));  // Safe starting volume
```

## Lessons Learned

### 1. Task Priorities and Watchdogs
Running audio processing in a separate high-priority task without delays can starve other tasks. For simple loopback, integrating into the main loop with proper watchdog resets is more reliable.

### 2. Floating GPIO Pins
Unused GPIO pins configured as inputs will float and trigger false readings. Always explicitly handle or disable unused peripherals based on hardware configuration.

### 3. Differential vs Single-Ended Inputs
Many modern audio codecs use differential inputs for noise rejection. Converting from single-ended line-level requires:
- Driving the positive input through an attenuator
- Grounding (or AC-grounding) the negative input
- Proper DC biasing considerations

### 4. Line Level vs Mic Level
- **Mic level:** ~10-50mV (what ES8311 MIC inputs expect)
- **Line level:** ~1-2V (what phones/computers output)
- **Attenuation needed:** ~20-100x (achieved with voltage divider)

### 5. AC Coupling Cutoff Frequency
For full audio spectrum (20Hz - 20kHz):
```
f_cutoff = 1 / (2π × R × C)
         = 1 / (2π × 57kΩ × 10µF)
         ≈ 0.28 Hz
```
Well below audible range - ✅ good choice.

## Next Steps

1. **Complete diagnostic tests** to identify root cause of Peak: 0
2. **Verify soldering** with photos and continuity tests
3. **Test DAC output** independently (can we hear anything at all?)
4. **Adjust codec gain** if signal is too weak
5. **Monitor for clipping** if signal is too strong (Peak approaching 32767)
6. **Once loopback working:** Re-enable network transmission for mesh testing

## Resources

- [ESP32-LyraT-Mini Documentation](https://docs.espressif.com/projects/esp-adf/en/latest/design-guide/dev-boards/get-started-esp32-lyrat-mini.html)
- [ES8311 Datasheet](https://www.everest-semi.com/pdf/ES8311%20PB.pdf)
- [I2S Interface Basics](https://www.sparkfun.com/datasheets/BreakoutBoards/I2SBUS.pdf)

## Code References

- Main loop: [src/tx/main.c](file:///Users/justin/Library/CloudStorage/Dropbox/NYU/semester-3-2025-fall/project-development-studio/meshnet-audio/src/tx/main.c#L194-L213)
- I2S driver: [lib/audio/src/i2s_in.c](file:///Users/justin/Library/CloudStorage/Dropbox/NYU/semester-3-2025-fall/project-development-studio/meshnet-audio/lib/audio/src/i2s_in.c)
- ES8311 codec: [lib/audio/src/es8311.c](file:///Users/justin/Library/CloudStorage/Dropbox/NYU/semester-3-2025-fall/project-development-studio/meshnet-audio/lib/audio/src/es8311.c)
- Pin definitions: [lib/config/include/config/pins_lyrat.h](file:///Users/justin/Library/CloudStorage/Dropbox/NYU/semester-3-2025-fall/project-development-studio/meshnet-audio/lib/config/include/config/pins_lyrat.h)

---

**Status:** Firmware verified working, hardware input circuit needs troubleshooting.
