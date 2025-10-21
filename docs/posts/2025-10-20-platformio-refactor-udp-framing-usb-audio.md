# PlatformIO Refactor, UDP Framing, and USB Audio Integration

## Where We Left Off

The last post ended with a rollback to basic mono tone generation after the stack overflow debacle. We had working UDP streaming - TX generating a 440Hz tone, broadcasting over WiFi, and RX receiving and playing it through the UDA1334 DAC. The audio quality was garbage, but the fundamental pipeline worked.

This week was about getting back on track with proper infrastructure - refactoring the build system to PlatformIO for better tooling, implementing packet framing for reliable UDP transport, and starting the push toward USB audio input so I could finally test with real music instead of that awful test tone.

## The PlatformIO Refactor: Build System Overhaul

The original ESP-IDF workspace with separate apps for TX and RX was getting unwieldy. Managing two separate build configurations, keeping dependencies in sync, and dealing with ESP-IDF's verbose build output was slowing down development.

PlatformIO promised a better development experience:
- Single unified project with multiple build environments
- Better library dependency management
- Integrated serial monitor and upload tools
- VS Code integration that actually works

### Project Structure Redesign

The new structure consolidates everything into a single PlatformIO project with two environments:

```
meshnet-audio/
├── platformio.ini          # Unified build config
├── src/
│   ├── tx/
│   │   └── main.c          # TX entry point
│   └── rx/
│       └── main.c          # RX entry point
├── lib/
│   ├── audio/              # Shared audio components
│   ├── network/            # WiFi mesh & UDP
│   ├── control/            # Display & buttons
│   └── config/             # Build constants & pins
├── sdkconfig.tx.defaults   # TX-specific ESP-IDF config
├── sdkconfig.rx.defaults   # RX-specific ESP-IDF config
└── extra_script.py         # Build-time script
```

Each environment in `platformio.ini` defines its target board, build flags, and sdkconfig:

```ini
[env:tx]
board = seeed_xiao_esp32s3
board_build.sdkconfig = sdkconfig.tx.defaults
upload_port = /dev/cu.usbmodem101

[env:rx]
board = seeed_xiao_esp32s3
board_build.sdkconfig = sdkconfig.rx.defaults
upload_port = /dev/cu.usbmodem2101
```

This means I can build and upload to both devices without switching configurations manually. Just `pio run -e tx -t upload` or `pio run -e rx -t upload`.

### Shared Library Architecture

The `lib/` directory contains components shared between TX and RX:

**`lib/audio/`**: Tone generation, I2S output, ring buffers, and (future) USB audio input  
**`lib/network/`**: WiFi AP/STA management, UDP broadcast/receive, latency measurement  
**`lib/control/`**: SSD1306 display rendering, button debouncing, status structs  
**`lib/config/`**: Centralized pin definitions and build constants

Each library has its own `library.json` and `CMakeLists.txt`, so PlatformIO knows how to build and link them. This modular structure means changes to, say, the network layer don't require rebuilding audio components.

### The extra_script.py Trick

Here's where it gets interesting. Since TX and RX use different source files (`src/tx/main.c` vs `src/rx/main.c`), but ESP-IDF's component registration expects sources in a single `src/CMakeLists.txt`, we need to generate that file dynamically.

The `extra_script.py` runs before every build and generates the appropriate CMakeLists based on which environment is being built:

```python
Import("env")

pio_env = env["PIOENV"]

if pio_env == "tx":
    app_sources = "tx/main.c tx/usb_hooks.c"
    cmake_requires = "REQUIRES tinyusb"
    message = "Building TX firmware"
else:
    app_sources = "rx/main.c"
    cmake_requires = ""
    message = "Building RX firmware"

print(message)

cmake_sources = ' '.join([f'"{s}"' for s in app_sources.split()])
cmake_content = f"""
idf_component_register(SRCS {cmake_sources} {cmake_requires})
"""

with open("src/CMakeLists.txt", "w") as f:
    f.write(cmake_content)
```

This auto-generates the right CMakeLists for each build target. TX includes USB-specific source files and requires the TinyUSB component; RX gets a simpler configuration.

### Migration Wins and Losses

**Wins:**
- Build times dropped significantly (incremental builds are much faster)
- Upload and monitor workflow is seamless with PlatformIO tasks
- Library dependency management is clearer
- Can easily switch between TX and RX builds

**Losses:**
- Lost some visibility into ESP-IDF's internal build process
- Had to learn PlatformIO's conventions on top of ESP-IDF's
- The `extra_script.py` dynamic generation adds a layer of indirection that's not immediately obvious when debugging

Overall, the refactor was absolutely worth it. Development velocity increased noticeably.

## Tier 1 Audio: The Stack Overflow Reality Check

Before diving into framing and USB, I attempted to implement proper stereo audio streaming. Updated the build configuration to 44.1kHz 16-bit stereo with 10ms frames:

```cpp
#define AUDIO_SAMPLE_RATE      44100
#define AUDIO_BITS_PER_SAMPLE  16
#define AUDIO_CHANNELS         2
#define AUDIO_FRAME_MS         10
```

The build succeeded. TX booted, initialized everything, and then crashed immediately with "A stack overflow in task main has been detected."

After incrementally increasing the main task stack from 8KB up to 64KB with no success, I traced the issue to ADC calibration functions consuming massive stack space. Combined with larger stereo audio buffers, the main task was blowing through even generous stack allocations.

The fix involved moving large audio buffers to global scope (per the development guidelines I'd been ignoring), but rather than chase that rabbit hole further, I made the pragmatic call to rollback to the last working commit and focus on infrastructure first.

Lesson learned: Memory management in embedded systems requires discipline up front. The "oracle" (development guidelines) was right about global buffers and careful stack management.

## The Packet Size Mystery

With the rollback complete and PlatformIO working smoothly, I started noticing weird behavior in the RX logs:

```
RX: Received packet with wrong size: 4 (expected 1764)
```

Four bytes? That's not an audio frame. That's a ping packet. The network layer had latency measurement built in using 4-byte ping packets, and those were showing up in the audio receive handler. Meanwhile, TX was logging that it was sending full 1764-byte audio frames every 10ms.

Two issues emerged:
1. Ping packets needed to be handled separately from audio packets
2. Those 1764-byte frames were probably getting fragmented by UDP/IP

## Reducing Frame Size: Avoiding Fragmentation

The frame size math was straightforward:
- 44.1kHz sample rate
- 16-bit (2 bytes) per sample
- 2 channels (stereo)
- 10ms frames

Result: 44100 samples/sec × 0.01 sec × 2 bytes × 2 channels = 1764 bytes per frame.

But UDP packets have a practical size limit around 1400-1500 bytes before IP fragmentation kicks in. Fragmentation is terrible for real-time audio - packets can arrive out of order, get lost, or introduce unpredictable latency.

The fix: reduce frame duration from 10ms to 5ms. This cuts the packet size roughly in half to about 882 bytes, well under the fragmentation threshold.

```cpp
// In lib/config/include/config/build.h
#define AUDIO_FRAME_MS 5  // Reduced from 10
```

More frequent transmissions, but each packet is atomic - no fragmentation, cleaner receive path, more predictable latency.

## Implementing Minimal Framing

At this point, raw PCM data was flying over UDP with no way to distinguish audio packets from ping packets, no sequence numbering, no error detection. Time to add a proper frame header.

I kept it minimal - we can migrate to Opus and RTP later, but for now we just needed basic packet identification:

```cpp
typedef struct __attribute__((packed)) {
    uint8_t magic;           // 0xA5
    uint8_t version;         // 1
    uint8_t type;            // NET_PKT_TYPE_AUDIO_RAW, NET_PKT_TYPE_PING, etc.
    uint8_t reserved;        // Future use
    uint16_t seq;            // Sequence number
    uint32_t timestamp;      // Milliseconds since boot
    uint16_t payload_len;    // Bytes of payload following header
} net_frame_header_t;
```

The header is 12 bytes and provides everything needed for robust streaming:
- **Magic byte (0xA5) and version**: Quick validation that this is a framed packet
- **Packet type**: Distinguishes audio from ping from future control messages
- **Sequence number**: Detects packet loss and reordering
- **Timestamp**: Enables jitter buffer management and latency measurement
- **Payload length**: Validates packet integrity

### TX Side: Building Framed Packets

On the transmitter, the process is straightforward - build the header, copy in the audio payload, send it:

```cpp
net_frame_header_t hdr;
hdr.magic = NET_FRAME_MAGIC;
hdr.version = NET_FRAME_VERSION;
hdr.type = NET_PKT_TYPE_AUDIO_RAW;
hdr.reserved = 0;
hdr.seq = htons(tx_seq++);
hdr.timestamp = htonl((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
hdr.payload_len = htons((uint16_t)AUDIO_FRAME_BYTES);

memcpy(framed_buffer, &hdr, sizeof(hdr));
memcpy(framed_buffer + sizeof(hdr), packet_buffer, AUDIO_FRAME_BYTES);

network_udp_send(framed_buffer, sizeof(hdr) + AUDIO_FRAME_BYTES);
```

Used `htons` and `htonl` for network byte order (big-endian) even though both ESP32s are little-endian. Good habits for future interoperability.

### RX Side: Parsing and Legacy Fallback

The receiver got more sophisticated. I wanted backward compatibility, so it checks for the frame header but also accepts raw payload-only packets (legacy mode):

```cpp
if (received_len >= NET_FRAME_HEADER_SIZE) {
    net_frame_header_t *hdr = (net_frame_header_t *)recv_buffer;
    
    if (hdr->magic == NET_FRAME_MAGIC && hdr->version == NET_FRAME_VERSION) {
        // Framed packet - parse it
        uint16_t payload_len = ntohs(hdr->payload_len);
        uint16_t seq = ntohs(hdr->seq);
        
        if (hdr->type == NET_PKT_TYPE_AUDIO_RAW) {
            // Extract payload and write to jitter buffer
            uint8_t *payload = recv_buffer + NET_FRAME_HEADER_SIZE;
            ring_buffer_write(jitter_buffer, payload, payload_len);
            ESP_LOGI(TAG, "Received framed audio seq=%u payload=%u bytes", seq, payload_len);
        }
    }
} else if (received_len == AUDIO_FRAME_BYTES) {
    // Legacy raw payload - accept it
    ring_buffer_write(jitter_buffer, recv_buffer, received_len);
    ESP_LOGI(TAG, "Received legacy raw audio %d bytes", received_len);
}
```

This approach meant I could test incrementally - deploy framed TX while keeping old RX running, verify the new code works, then update RX.

## Debugging On-Wire Packet Contents

Even with framing in place, RX logs showed confusing validation warnings:

```
Audio frame size mismatch: payload_len=880 received_len=880 seq=123
```

The lengths matched, so why the warning? I needed to see what was actually arriving on the wire. Added hex dumps of the first 16 bytes of every received UDP packet:

```cpp
ESP_LOGI(TAG, "UDP recv %d bytes from %s:%d", len, ip, port);
ESP_LOG_BUFFER_HEX_LEVEL(TAG, buf, (len > 16 ? 16 : len), ESP_LOG_INFO);
```

This revealed:
1. Frame header was present and correct (0xA5 magic byte visible)
2. Sequence numbers were incrementing properly
3. Payload length field matched received length
4. The "mismatch" warnings were overly aggressive validation code

Cleaned up the logging to only warn on actual problems. The hex dumps were invaluable for understanding wire-level behavior versus application-level assumptions.

## USB Audio: The Enumeration Quest

With framing working and audio streaming reliably, it was time to tackle USB audio input. The goal: plug TX into my Mac, have it show up as an audio output device, and stream whatever's playing (Spotify, YouTube, whatever) over the mesh.

No more testing with that harsh 440Hz sine wave.

### TinyUSB and the USB Device UAC Component

The project already had `espressif__tinyusb` and `espressif__usb_device_uac` as managed components. The UAC (USB Audio Class) component provides a high-level API for implementing USB audio devices on ESP32-S3.

I updated `sdkconfig.tx.defaults` to enable USB audio:

```ini
CONFIG_TINYUSB_ENABLED=y
CONFIG_USB_UAC_ENABLED=y
CONFIG_USB_UAC_CHANNELS_NUM=2
CONFIG_USB_UAC_SAMPLE_RATE_44100=y
```

Then initialized it in `tx/main.c`:

```cpp
ESP_ERROR_CHECK(usb_audio_init());
```

The USB audio library provides a callback that fires when the host sends audio data:

```cpp
void usb_audio_output_cb(const uint8_t *buf, size_t len) {
    // Write incoming USB audio to ring buffer
    ring_buffer_write(usb_ring_buffer, buf, len);
}
```

In theory, the TX device should now enumerate as a USB audio device, receive audio from the host, buffer it in the ring buffer, and the main loop would read from that buffer and broadcast it over UDP.

### The Build System Rabbit Hole

The build failed immediately:

```
undefined reference to `usb_debug_start'
```

I had added USB mount/unmount callbacks for debugging:

```cpp
// src/tx/usb_hooks.c
void tud_mount_cb(void) {
    ESP_LOGI(TAG, "USB mounted (host connected)");
}

void tud_umount_cb(void) {
    ESP_LOGI(TAG, "USB unmounted (host disconnected)");
}

void usb_debug_start(void) {
    xTaskCreate(usb_debug_task, "usb_debug", 2048, NULL, 5, NULL);
}
```

And called `usb_debug_start()` from `tx/main.c`, but the linker couldn't find it.

The issue traced back to the `extra_script.py` that generates `src/CMakeLists.txt` dynamically. It was only including `tx/main.c` for TX builds, not `tx/usb_hooks.c`.

Fixed it by updating the script to include multiple source files:

```python
if pio_env == "tx":
    app_sources = "tx/main.c tx/usb_hooks.c"
    message = "Building TX firmware"
```

And adjusted the CMake generation to split sources properly:

```python
cmake_sources = ' '.join([f'"{s}"' for s in app_sources.split()])
cmake_content = f"""
idf_component_register(SRCS {cmake_sources})
"""
```

Build got further, then failed again:

```
fatal error: tusb.h: No such file or directory
```

The TinyUSB component wasn't being included. The generated `CMakeLists.txt` only listed source files, no component dependencies. Fixed by adding a `REQUIRES` clause:

```python
if pio_env == "tx":
    app_sources = "tx/main.c tx/usb_hooks.c"
    cmake_requires = "REQUIRES tinyusb"
else:
    app_sources = "rx/main.c"
    cmake_requires = ""

cmake_sources = ' '.join([f'"{s}"' for s in app_sources.split()])
cmake_content = f"""
idf_component_register(SRCS {cmake_sources} {cmake_requires})
"""
```

This tells ESP-IDF's build system to include TinyUSB's headers and link its libraries when building TX firmware.

After this, the build succeeded.

## Current State and Next Steps

As of now:

✅ **PlatformIO refactor complete** - unified build system working smoothly  
✅ **Frame header implemented** - 12-byte header with magic, version, type, seq, timestamp, payload_len  
✅ **TX side framing** - building and sending framed packets with sequence numbers  
✅ **RX side parsing** - receiving framed packets with legacy fallback  
✅ **Packet size optimized** - 5ms frames (~880 bytes) avoid IP fragmentation  
✅ **USB audio compiled in** - TX firmware includes TinyUSB and USB Device UAC  
✅ **Debug instrumentation** - hex dumps and mount/unmount callbacks

**Pending:**
- **USB enumeration testing** - flash TX and verify it shows up on Mac as audio device
- **End-to-end audio test** - play Spotify through the system
- **Sequence loss detection** - use sequence numbers to detect and log dropped packets
- **Jitter buffer tuning** - analyze latency and optimize receive buffering

The immediate test is straightforward:
1. Flash updated TX firmware
2. Plug TX into Mac via USB
3. Check System Information → USB to see if device appears
4. Check Audio MIDI Setup for new audio device
5. Monitor TX serial output for "USB mounted? YES" messages

If enumeration works, I can finally test the system with real audio. If not, it's time to dive into USB descriptors, endpoint configurations, and probably some logic analyzer traces of the USB bus.

## Lessons Learned

### PlatformIO Build System Is Worth The Migration

The initial investment in restructuring the project paid off immediately. Faster builds, better tooling integration, clearer dependency management. The `extra_script.py` approach for generating environment-specific CMake files is powerful once you understand it.

### Incremental Debugging Tools Are Essential

Hex dumps of packet headers were invaluable for understanding wire-level behavior. Network code needs instrumentation at multiple layers - application logs, packet dumps, and eventually network captures with Wireshark.

### Framing Overhead Is Negligible

The 12-byte header represents ~1.3% overhead on an 880-byte frame. That's nothing compared to the benefits: packet type identification, sequence numbering, timestamp information, and payload validation. The bandwidth cost is absolutely worth the robustness.

### Legacy Compatibility Enables Incremental Testing

Building in backward compatibility (accepting raw payload packets) meant testing the new framing code without breaking the existing system. Deploy incrementally, validate each step, move forward.

### Build System Archaeology Takes Time

When linker errors happen in a complex build setup (ESP-IDF + PlatformIO + dynamic CMake generation), tracing through multiple layers to find the root cause requires patience and systematic investigation. Understanding the build system is as important as understanding the code.

## The Path Forward

The hardware is capable. The network layer is solid. Framing is in place. USB audio is compiled in. Now it's time to prove the whole pipeline works with actual audio content.

If USB enumeration succeeds, this project crosses a major threshold - from "streaming a test tone" to "wireless audio distribution system for real music." That's the difference between a proof of concept and something genuinely useful.

And if it doesn't enumerate cleanly, well, that's another debugging adventure. But either way, the infrastructure is now in place to support high-quality audio streaming. The foundation is solid enough to build on.
