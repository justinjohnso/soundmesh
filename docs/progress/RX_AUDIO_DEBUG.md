# RX Audio Not Playing - Debugging Guide

## Status from Previous Thread
- Tone test mode confirmed I2S writes were succeeding (`ret=0`)
- But **no audio heard** - only static
- **Power was switched to 5V** to make it work at all
- This points to **hardware power/supply issue** as root cause

## Immediate Actions to Debug

### Step 1: Verify Power Supply
**Most likely culprit:** UDA1334 getting insufficient power

**Action:**
- Measure voltage on UDA1334 VIN pin:
  - Should be **5V** (not 3.3V from XIAO)
  - At 3.3V, output is only ~900mV RMS (very quiet)
  - At 5V, output is ~1.5V RMS (proper line level)
  
**Fix if needed:**
```
Option A: Use XIAO 5V output if available
Option B: Use external 5V supply for UDA1334 VIN
Option C: Stick with 3.3V but increase software volume scaling (RX_OUTPUT_VOLUME)
```

### Step 2: Verify I2S Signal Integrity
**To confirm BCK/WS/DIN signals are correct:**

Enable oscilloscope/logic analyzer on:
- GPIO7 (I2S_BCK) - bit clock ~1.5MHz @ 48kHz
- GPIO8 (I2S_WS) - word select ~48kHz square wave
- GPIO9 (I2S_DO) - digital audio data

**Expected:**
- BCK: ~1.5MHz continuous clock
- WS: 48kHz square wave (left/right select)
- DIN: Data synchronized to BCK edges

### Step 3: Enable Diagnostic Logging in adf_pipeline.c
The playback task has good logging (lines 790-796). Run with this enabled:

```bash
pio run -e rx
# Monitor with:
pio device monitor -b 115200 | grep -E "Playback|decode|I2S write"
```

**Look for:**
- "Playback #1: s[0]=... s[100]=... s[500]=..." - decoded samples
- "I2S write #..." - I2S writes succeeding
- "RX decode task started" - decoder initialized
- "RX playback task started" - playback started

If you see these but no audio, **it's a hardware issue**.

### Step 4: Test with Pure Tone Mode
In `adf_pipeline.c` line 719, change:
```c
#define RX_TEST_TONE_MODE 0  // Change to 1
```

This **bypasses the entire audio pipeline** and generates a 440Hz tone directly to I2S.

**If tone works:**
- I2S hardware is fine
- Problem is in audio pipeline (decoder, buffers, etc.)

**If tone doesn't work:**
- Hardware issue (wiring, power, I2S config)

### Step 5: Check Volume Scaling
In `adf_pipeline.c` lines 799, the playback path writes directly:
```c
i2s_audio_write_mono_as_stereo(mono_frame, AUDIO_FRAME_SAMPLES);
```

There is **no volume scaling** applied here (unlike TX which has volume control).

**If audio is too quiet:**
- Add volume scaling before I2S write
- Or increase UDA1334 power supply to 5V

---

## Recommended Fix Order

### Priority 1: Power Supply (99% likely to fix)
```
RX UDA1334 VIN must be 5V minimum
Current: probably 3.3V → output ~900mV (barely audible)
Target: 5V → output ~1.5V (standard line level)
```

### Priority 2: Test Tone Mode
```c
// In adf_pipeline.c, line 719:
#define RX_TEST_TONE_MODE 1  // Enable bypass test

// Build and verify:
pio run -e rx
// Should hear 440Hz tone from headphones
// If yes: audio path works, problem is in pipeline
// If no: hardware issue (wiring, I2S config, power)
```

### Priority 3: Check Decoder Output
```c
// In adf_pipeline.c, line 691:
if (first_pcm_log && samples_decoded > 0) {
    first_pcm_log = false;
    ESP_LOGI(TAG, "First decoded frame: samples=%d, s[0]=%d, s[1]=%d",
             samples_decoded, (int)pcm_frame[0], (int)pcm_frame[1]);
}
// Add more detail:
if (samples_decoded <= 0) {
    ESP_LOGE(TAG, "DECODER FAILED: returned %d samples", samples_decoded);
}
```

---

## Hardware Wiring Checklist

**UDA1334 → XIAO ESP32-S3:**
- [ ] VIN → 5V (NOT 3.3V!) ← **CRITICAL**
- [ ] GND → GND
- [ ] BCK (pin 1) → GPIO7
- [ ] WS (pin 2) → GPIO8  
- [ ] DIN (pin 3) → GPIO9
- [ ] Headphones → L/R audio outputs + AGND

**XIAO 5V Supply:**
- [ ] XIAO has 5V output? (check product specs)
- [ ] If not, use external 5V regulated supply
- [ ] Share GND between XIAO and 5V supply

---

## Diagnostics to Run

```bash
# Build with logging
pio run -e rx

# Monitor output filtering for audio-related messages
pio device monitor -b 115200 | grep -v "mesh\|network\|wifi"

# Look for these specific lines:
# - "RX playback task started"
# - "First decoded frame: samples=..."
# - "Playback #1: s[0]=..."
# - Any errors or warnings
```

---

## Next Steps After Testing

1. **If tone test works** → decoder/opus issue
   - Check opus_decode return values in adf_pipeline.c:678
   - Verify opus_buffer has valid data
   - Check sample rate mismatch (48kHz expected)

2. **If tone test fails** → hardware issue
   - Verify 5V power on UDA1334
   - Check I2S wiring (GPIO7/8/9)
   - Measure clock signals with oscilloscope

3. **If audio is quiet** → volume issue
   - Increase UDA1334 power to 5V
   - Or add software volume scaling before I2S write

---

## Reference: UDA1334 Key Facts

From Adafruit + NXP datasheet:
- **MCLK is NOT required** - built-in PLL generates clock from WS
- **Power:** 3.3V minimum, 5V recommended for proper output level
- **Output impedance:** 3kΩ load rated (32Ω headphones work but with distortion)
- **I2S Format:** Standard (BCK must align with WS negative edge)
- **No control pins needed** - just BCK/WS/DIN/Power/GND

The old "working" implementations all had one thing in common:
**They ran the UDA1334 on 5V power, not 3.3V.**
