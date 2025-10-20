# Tier 1 Audio Quality: Stack Overflow Edition

Hey, remember that ambitious plan from a couple weeks ago where I outlined this three-tier approach to high-quality audio streaming? Tier 1: CD quality 44.1kHz 16-bit stereo, Tier 2: 96kHz 24-bit, and Tier 3: 192kHz 24-bit? Yeah, that was the plan. And then reality hit like a stack overflow in the face.

## The Original Vision (Brief Recap)

We were rolling with a solid baseline - TX and RX devices talking over WiFi, basic tone generation working, display showing network stats. The goal was to incrementally boost audio quality from the current mono tone up to uncompressed PCM streaming that could handle serious bandwidth (targeting ~9.22Mbps for 96kHz 24-bit stereo).

## Tier 1: The Stack Overflow Debacle

I dove in with confidence, updating the build configuration in `lib/config/include/config/build.h`:

```cpp
#define AUDIO_SAMPLE_RATE      44100
#define AUDIO_BITS_PER_SAMPLE  16
#define AUDIO_CHANNELS         2
#define AUDIO_FRAME_MS         10
```

Simple enough, right? 44.1kHz stereo with 10ms frames means about 882 samples per frame. The math checked out, and the build succeeded. But then the TX device started panicking immediately on boot.

The logs showed the classic embedded developer nightmare: "A stack overflow in task main has been detected." The device would initialize everything - display, buttons, WiFi AP, tone generator, ring buffer - and then just crash right as it tried to enter the main loop.

I spent way too long staring at this, incrementally increasing the main task stack size from the default 8KB up to 32KB, then 64KB. Nothing worked. The crash was happening right after "Tone frequency updated to X Hz", which pointed to the ADC reading code in the main loop.

## ADC: The Hidden Villain

The main loop was doing ADC reads every 10ms to control tone frequency via a potentiometer:

```cpp
int adc_raw;
ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, ADC_CHANNEL_2, &adc_raw));

int voltage_mv;
ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc1_cali_handle, adc_raw, &voltage_mv));
```

Turns out those ADC calibration functions were consuming massive amounts of stack space. Combined with the larger audio buffers (882 samples for stereo), the main task was blowing through even 64KB of stack.

## The Rollback Reality Check

After a solid day of debugging, I made the tough call to rollback. We reset to commit `3cf80e0` - the last known working build with basic mono tone generation. It's not pretty, but it works. TX creates an AP, RX connects, and we get audio streaming... of a sort.

## Current State: Garbage-Sounding Tone Generation

Right now, we're back to basics:
- 44.1kHz sample rate
- 16-bit samples  
- Mono audio (single channel)
- 10ms frames
- Tone generator producing a fixed-frequency sine wave
- UDP broadcasting on port 3333

The audio quality? Let's be honest - it's garbage. It's that harsh, buzzy digital tone that makes you question your life choices. But hey, it works end-to-end. TX generates, broadcasts, RX receives and outputs via I2S to the UDA1334 DAC.

## The Path Forward (Oracle-Approved Strategy)

Consulting the development guidelines, the right approach is systematic mitigation before charging ahead:

1. **Memory Management First**: Move those large audio buffers (`mono_frame`, `audio_frame`, `packet_buffer`) to global scope to reduce stack pressure
2. **Isolate ADC Processing**: Extract ADC reading/calibration to a separate task, not in the tight audio loop
3. **Simplify Error Handling**: Replace heavy `ESP_ERROR_CHECK` macros with lightweight checking
4. **Add System Protection**: Implement watchdog timer for crash recovery
5. **Incremental Testing**: Build and test each change independently

## Lessons Learned

- Stack overflows in embedded systems are brutal and opaque
- ADC calibration libraries can be stack hogs
- Always have a working rollback point
- The "oracle" (development guidelines) was right about memory management being critical
- Sometimes you need to step back to move forward

We're not done with high-quality audio yet, but we have a stable foundation now. Next up: implementing those oracle-recommended fixes and actually getting to Tier 1 stereo streaming.
