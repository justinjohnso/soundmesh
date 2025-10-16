# MeshNet Audio: MVP Refactor & Streaming Milestone (2025-10-15)

## Display/UI Refactor: Why and How

The original SSD1306 display code was inflexible and made it hard to animate or show dynamic text. Swapped it out for Adafruit_SSD1306 (C version for ESP-IDF), which made it way easier to do waveform animations and text overlays. Both TX and RX now have two views each, toggled by the button on GPIO4:
- TX: (1) Streaming waveform, (2) RX node count (simulated)
- RX: (1) Mesh latency/hops (simulated), (2) Streaming waveform
Button debounce is handled in software, and the display updates every ~100ms.

## Mesh Stats: Simulated for Now

Mesh stats (latency, hops, RX node count) are stubbed in both TX and RX. For now, TX increments a fake RX node count every 5s, and RX cycles latency/hops values. The display logic is modular so real mesh stats can be dropped in later when the mesh transport is ready.

## Audio Input/Output: Streaming Over UDP

### TX Side
- Added audio input mode selection: TONE (default), AUX (ADC stub), USB (stub).
- Button long-press toggles input mode; short press cycles display views.
- UDP sender streams selected audio input (tone, simulated AUX, or silence for USB).
- Display shows current input mode.

### RX Side
- Replaced PWM output with I2S for UDA1334 DAC.
- I2S initialized for 16kHz, 16-bit stereo output.
- Received UDP packets are played via I2S (real DAC output).
- Display/UI logic matches TX for consistency.

## Technical Decisions & Lessons
- Adafruit_SSD1306 is way easier for dynamic UI than raw SSD1306 page addressing.
- Button logic is unified for both display and input mode toggling.
- Mesh stats and audio input/output are stubbed for MVP, but code is structured for easy upgrade.
- UDP broadcast is used for now; mesh_stream and ctrl_plane are ready for future integration.

## Next Steps
- Integrate real mesh stats and audio transport when mesh_stream is ready.
- Replace ADC/USB stubs with real drivers (PCF8591, TinyUSB UAC).
- Continue documenting technical pivots and lessons in this folder.

---

*References: See previous posts for OLED display logic, mesh stats planning, and hardware integration details.*
