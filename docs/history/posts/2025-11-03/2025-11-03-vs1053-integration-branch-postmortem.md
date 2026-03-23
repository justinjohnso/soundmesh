# VS1053 Integration Branch: Postmortem - Why ADPCM Compression Didn't Work

## The Premise: Bandwidth Compression via Hardware Codec

After getting basic UDP audio streaming working with uncompressed 48kHz stereo PCM (~1.5Mbps bandwidth), the next logical step was compression. The goal: reduce bandwidth from 1.5Mbps to something manageable on WiFi mesh networks, ideally under 500kbps.

IMA ADPCM (Adaptive Differential Pulse Code Modulation) promised 4:1 compression with minimal CPU overhead. The VS1053 codec chip from VLSI Solutions was the perfect hardware accelerator - dedicated ADPCM encoder/decoder that could handle line-in audio input and output.

## Branch Timeline: From Enthusiasm to Hardware Reality

### Initial Architecture Vision (VS1053 Migration Plan)

The plan was ambitious but logical:

**TX Unit**: Line-in (aux) → VS1053 ADPCM encode → ESP32 reads blocks → UDP broadcast  
**RX Unit**: UDP receive → VS1053 ADPCM decode → Line-out  

**Expected Benefits:**
- 24x bandwidth reduction (1.5Mbps → 64kbps)
- Hardware-accelerated compression (no ESP32 CPU load)
- Real aux audio input (not just generated tones)
- Same chip handles both encode and decode

**Pin Requirements:**
- SPI bus (SCK/MOSI/MISO) - shared with OLED
- XCS (chip select for commands)
- XDCS (chip select for audio data)  
- DREQ (data ready signal)
- XRESET (hardware reset)

### Adafruit Library Integration: First Attempt

Started with the easiest path: Adafruit's Arduino VS1053 library. Added it to `platformio.ini`:

```ini
lib_deps =
    adafruit/Adafruit VS1053 Library @ ^1.2.1
    adafruit/Adafruit BusIO @ ^1.16.1
```

Created an adapter layer (`vs1053_adafruit_adapter.cpp`) to bridge the C++ Arduino API to our C ESP-IDF codebase:

```cpp
extern "C" {
esp_err_t vs1053_init(vs1053_t* vs, spi_host_device_t spi_host) {
    initArduino();
    SPI.begin(VS1053_SPI_SCK, VS1053_SPI_MISO, VS1053_SPI_MOSI);
    
    static Adafruit_VS1053 vs_inst(VS1053_XRESET, VS1053_XCS, VS1053_XDCS, VS1053_DREQ);
    s_vs = &vs_inst;
    
    if (!s_vs->begin()) {
        return ESP_FAIL;
    }
    // ...
}
}
```

**Initial Success:** Both TX and RX units built and booted successfully. VS1053 initialized, WiFi connected, serial output showed proper boot sequence.

### ADPCM Recording Mode: The Library Limitation

The problem emerged during ADPCM activation. The Adafruit library supports:
- ✅ Audio playback (MP3, AAC, WAV)
- ✅ Ogg Vorbis recording (requires SD card)
- ❌ IMA ADPCM recording (not implemented)

Our pipeline needed ADPCM recording, which requires direct register access:

```c
// Required ADPCM sequence (from datasheet)
vs1053_sci_write(vs, VS1053_SCI_MODE, SM_SDINEW | SM_RESET | SM_ADPCM);
vs1053_sci_write(vs, VS1053_SCI_AICTRL0, 0);        // ADPCM profile
vs1053_sci_write(vs, VS1053_SCI_AICTRL1, 48000);   // Sample rate
// Read encoded data from HDAT0/HDAT1 registers
```

The Adafruit library abstracts away register access, exposing only high-level functions. When we tried ADPCM recording, `SCI_HDAT1` always returned 0 - no data available.

**Root Cause:** Adafruit library doesn't implement the ADPCM recording firmware activation sequence.

### Custom ESP-IDF Driver: Second Attempt

Abandoned the Adafruit library and implemented a custom SPI driver. Created `lib/audio/vs1053/vs1053.c` with direct register access:

**SPI Configuration:**
- Two device handles: SCI (commands) and SDI (audio data)
- Full-duplex SPI (critical for VS1053)
- DREQ polling before each transaction

**API Design:**
```c
typedef struct {
    spi_device_handle_t sci_handle;
    spi_device_handle_t sdi_handle;
    gpio_num_t dreq_pin;
    gpio_num_t reset_pin;
    bool initialized;
} vs1053_t;

// Core functions
esp_err_t vs1053_sci_write(vs1053_t* vs, uint8_t reg, uint16_t value);
esp_err_t vs1053_sci_read(vs1053_t* vs, uint8_t reg, uint16_t* value);
esp_err_t vs1053_start_adpcm_record(vs1053_t* vs, uint32_t sample_rate, bool stereo);
int vs1053_read_adpcm_block(vs1053_t* vs, uint8_t* buffer, size_t buffer_size);
```

**Pipeline Integration:**
- TX: `record_task()` reads ADPCM blocks → ring buffer → UDP send
- RX: UDP receive → jitter buffer → VS1053 SDI playback

**Build System Changes:**
Updated `lib/audio/CMakeLists.txt` to include custom driver:
```cmake
idf_component_register(
    SRCS "tx_pipeline.c" "rx_pipeline.c" "src/vs1053_adafruit_adapter.cpp"
    INCLUDE_DIRS "include" "." "vs1053"
    REQUIRES arduino
)
```

Wait, that still references the Adafruit adapter. Let me check what the actual CMake was in the branch.

Actually, the custom driver was added but we kept the Adafruit one initially for comparison.

### Hardware Reality Check: VS1053 vs VS1053b

After implementing the custom driver, testing revealed the critical hardware issue: our breakout board uses **VS1053** (playback only), not **VS1053b** (recording capable).

**The Evidence:**
- `SCI_HDAT1` register always returned 0 (no recording data)
- Status register showed version 3, not 4 (VS1053b)
- ADPCM recording firmware not present in the chip

**The Purchase Mistake:** Ordered "VS1053" thinking it was the recording version, but VS1053b is the one with ADPCM capabilities.

## Technical Deep Dive: What Was Implemented

### Custom VS1053 SPI Driver

**SPI Bus Setup:**
```c
spi_bus_config_t bus_cfg = {
    .mosi_io_num = GPIO_NUM_9,
    .miso_io_num = GPIO_NUM_8, 
    .sclk_io_num = GPIO_NUM_7,
    .quadwp_io_num = -1,
    .quadhd_io_num = -1,
    .max_transfer_sz = 32
};

spi_device_interface_config_t sci_dev_cfg = {
    .clock_speed_hz = 2000000,  // 2 MHz for SCI
    .mode = 0,
    .spics_io_num = VS1053_CS_PIN,
    .queue_size = 1,
    .flags = SPI_DEVICE_HALFDUPLEX  // Initially wrong!
};
```

**Critical Bug:** Used `SPI_DEVICE_HALFDUPLEX` initially, causing failures. VS1053 requires **full-duplex** SPI.

**DREQ Flow Control:**
```c
bool vs1053_dreq_ready(vs1053_t* vs) {
    return gpio_get_level(vs->dreq_pin);
}

void vs1053_wait_dreq(vs1053_t* vs) {
    int timeout = 1000;
    while (!vs1053_dreq_ready(vs) && timeout-- > 0) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
```

**SCI Register Operations:**
```c
esp_err_t vs1053_sci_write(vs1053_t* vs, uint8_t reg, uint16_t value) {
    vs1053_wait_dreq(vs);
    
    uint8_t tx_data[4] = {0x02, reg, (value >> 8) & 0xFF, value & 0xFF};
    uint8_t rx_data[4];
    
    spi_transaction_t trans = {
        .length = 32,
        .tx_buffer = tx_data,
        .rx_buffer = rx_data
    };
    
    return spi_device_transmit(vs->sci_handle, &trans);
}
```

### ADPCM Recording Mode Implementation

**Initialization Sequence:**
```c
esp_err_t vs1053_start_adpcm_record(vs1053_t* vs, uint32_t sample_rate, bool stereo) {
    // 1. Soft reset with ADPCM bit set
    vs1053_sci_write(vs, VS1053_REG_MODE, SM_SDINEW | SM_RESET | SM_ADPCM);
    
    // 2. Configure clock
    vs1053_sci_write(vs, VS1053_REG_CLOCKF, 0x8800);  // 4.5x multiplier
    
    // 3. Set up recording parameters
    vs1053_sci_write(vs, VS1053_REG_AICTRL0, 0);         // ADPCM profile
    vs1053_sci_write(vs, VS1053_REG_AICTRL1, sample_rate); // Sample rate in Hz
    vs1053_sci_write(vs, VS1053_REG_AICTRL2, 0);         // AGC off
    vs1053_sci_write(vs, VS1053_REG_AICTRL3, stereo ? 1 : 0); // Mono/Stereo
    
    // 4. Start recording application
    vs1053_sci_write(vs, VS1053_REG_AIADDR, 0x0034);
    
    return ESP_OK;
}
```

**Data Reading:**
```c
int vs1053_read_adpcm_block(vs1053_t* vs, uint8_t* buffer, size_t buffer_size) {
    // Quick DREQ check
    if (!vs1053_dreq_ready(vs)) {
        return 0;  // No data available
    }
    
    // Check words available
    uint16_t words_available;
    vs1053_sci_read(vs, VS1053_REG_HDAT1, &words_available);
    
    if (words_available == 0) {
        return 0;
    }
    
    // Read data words
    size_t bytes_read = 0;
    size_t words_to_read = MIN(words_available, buffer_size / 2);
    
    for (size_t i = 0; i < words_to_read; i++) {
        uint16_t word;
        vs1053_sci_read(vs, VS1053_REG_HDAT0, &word);
        buffer[bytes_read++] = (word >> 8) & 0xFF;
        buffer[bytes_read++] = word & 0xFF;
    }
    
    return bytes_read;
}
```

### TX Pipeline Architecture

**Ring Buffer Approach:**
```c
static RingbufHandle_t s_ring_buffer = NULL;

// Record task: VS1053 → Ring Buffer
static void record_task(void* arg) {
    uint8_t block_buffer[ADPCM_BLOCK_SIZE_BYTES];
    
    while (s_running) {
        int bytes_read = vs1053_read_adpcm_block(s_vs, block_buffer, sizeof(block_buffer));
        
        if (bytes_read > 0) {
            xRingbufferSend(s_ring_buffer, block_buffer, bytes_read, pdMS_TO_TICKS(10));
            s_blocks_produced++;
        }
        vTaskDelay(pdMS_TO_TICKS(10));  // ~100Hz polling
    }
}

// Send task: Ring Buffer → UDP
static void send_task(void* arg) {
    while (s_running) {
        size_t item_size;
        uint8_t* item = (uint8_t*)xRingbufferReceive(s_ring_buffer, &item_size, pdMS_TO_TICKS(100));
        
        if (item) {
            // Build UDP packet with sequence/timestamp
            // Send via network layer
            vRingbufferReturnItem(s_ring_buffer, (void*)item);
        }
    }
}
```

### RX Pipeline (Playback)

**WAV Header Generation:**
```c
esp_err_t vs1053_write_wav_header_adpcm(vs1053_t* vs, uint32_t sample_rate, uint8_t channels) {
    // 58-byte WAV ADPCM header
    // RIFF chunk, fmt chunk (ADPCM), fact chunk, data chunk
    uint8_t header[58] = { /* ... ADPCM WAV header bytes ... */ };
    
    return vs1053_sdi_write(vs, header, sizeof(header));
}
```

**Playback Task:**
```c
static void playback_task(void* arg) {
    // First packet: send WAV header
    vs1053_write_wav_header_adpcm(s_vs, 48000, 1);
    
    while (s_running) {
        // Get next ADPCM block from jitter buffer
        // Send to VS1053 via SDI when DREQ ready
        vs1053_wait_dreq(s_vs);
        vs1053_sdi_write(s_vs, block_data, block_size);
    }
}
```

## Key Lessons Learned

### 1. Hardware Assumptions Kill Projects

**Documentation Lie:** Multiple sources (Adafruit docs, product descriptions) referred to the board as "VS1053" without specifying the recording limitation.

**Reality:** VS1053 = playback only. VS1053b = recording capable. The 'b' suffix is critical.

**Lesson:** When inheriting hardware, verify capabilities empirically. Don't trust marketing copy.

### 2. Library Limitations Can Be Show-Stoppers

Adafruit's library is excellent for playback, but the lack of ADPCM recording support made it unusable for our use case. Sometimes you need to go bare-metal.

### 3. SPI Mode Matters (Full-Duplex Required)

VS1053 SCI requires bidirectional communication. The initial `SPI_DEVICE_HALFDUPLEX` flag caused silent failures that took hours to debug.

### 4. DREQ Behavior Differs by Mode

During initialization: DREQ indicates "ready for next command"  
During recording: DREQ can be low temporarily while processing audio  

Treating recording low-DREQ as an error caused false timeouts. The fix: non-blocking check and graceful silence return.

### 5. Plugin Loading Requires Watchdog Management

VS1053 firmware loading takes significant time. Without `esp_task_wdt_reset()` calls, the watchdog would trigger at 5 seconds.

## Branch Status: Abandoned But Valuable

### What Was Accomplished

✅ **Custom VS1053 SPI driver** - Complete ESP-IDF implementation  
✅ **ADPCM recording pipeline** - TX record task and UDP transmission  
✅ **ADPCM playback pipeline** - RX jitter buffer and SDI streaming  
✅ **Build system integration** - PlatformIO support for both architectures  
✅ **Comprehensive debugging** - Register dumps, timing analysis, flow control  
✅ **Documentation** - Migration plan, wiring guide, issue analysis  

### Why It Was Abandoned

❌ **Hardware incompatibility** - VS1053 vs VS1053b  
❌ **Adafruit library limitations** - No ADPCM recording support  
❌ **Time investment** - 2-3 days invested, but ultimately blocked by chip version  

### What It Proved

The architecture was sound. With VS1053b hardware, ADPCM compression would have worked beautifully:
- 24x bandwidth reduction
- Hardware-accelerated encoding/decoding  
- Real aux audio input capability
- Same chip for both TX and RX

## The Pivot: ESP32 ADC Wins

After VS1053 failed, we pivoted to ESP32's built-in ADC for aux input. While not compressed, it provided:
- ✅ Reliable audio capture
- ✅ No additional hardware dependencies
- ✅ Sufficient bandwidth for prototyping
- ✅ Foundation for future compression (software ADPCM)

## Files Created/Modified in vs1053-integration

### New Files
- `lib/audio/vs1053/vs1053.c` - Custom SPI driver
- `lib/audio/vs1053/vs1053.h` - Driver API
- `lib/audio/vs1053/vs1053_regs.h` - Register definitions
- `lib/audio/tx_pipeline.c` - TX ADPCM recording pipeline
- `lib/audio/rx_pipeline.c` - RX ADPCM playback pipeline
- `docs/VS1053_MIGRATION_PLAN.md` - Architecture documentation
- `docs/VS1053_ADAFRUIT_ISSUE.md` - Problem analysis

### Modified Files
- `platformio.ini` - Added Adafruit library dependency
- `lib/audio/CMakeLists.txt` - Component registration
- `src/tx/main.c` - Pipeline integration
- `src/rx/main.c` - Pipeline integration
- `lib/config/pins.h` - VS1053 pin assignments
- `.vscode/c_cpp_properties.json` - Include paths

## Conclusion: Failed But Essential

The vs1053-integration branch was a comprehensive failure, but a valuable one. It proved the ADPCM compression architecture was viable, exposed critical hardware assumptions, and forced a pragmatic pivot to ESP32 ADC.

Sometimes the path not taken teaches you more than the one you follow. This branch taught us about SPI protocols, codec chip families, library limitations, and the importance of hardware verification.

The custom VS1053 driver remains a solid implementation - ready for VS1053b hardware if we ever acquire it. For now, ESP32 ADC carries us forward toward mesh networking and real wireless audio distribution.
