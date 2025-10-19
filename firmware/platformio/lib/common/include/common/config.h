#pragma once

// Network configuration
#define MESHNET_SSID "MeshAudioAP"
#define MESHNET_PASS "meshpass123"
#define MESHNET_CHANNEL 6
#define MESHNET_UDP_PORT 3333

// Audio configuration
#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_CHANNELS 1
#define AUDIO_BIT_DEPTH 16
#define AUDIO_SAMPLES_PER_PACKET 160  // 10ms @ 16kHz
#define AUDIO_PACKET_INTERVAL_MS 10

// I2C configuration
#define I2C_MASTER_SCL_IO 6
#define I2C_MASTER_SDA_IO 5
#define I2C_MASTER_NUM I2C_NUM_0
#define I2C_MASTER_FREQ_HZ 400000
#define OLED_I2C_ADDR 0x3C

// Button configuration
#define BUTTON_GPIO 4
#define BUTTON_DEBOUNCE_MS 50
#define BUTTON_LONG_PRESS_MS 1000

// I2S configuration (RX only)
#define I2S_BCK_IO 7
#define I2S_WS_IO 8
#define I2S_DATA_OUT_IO 9

// PCF8591 ADC configuration (TX only)
#define PCF8591_ADDR 0x48
#define PCF8591_CHANNEL 0

// Jitter buffer configuration
#define JITTER_BUFFER_PACKETS 4
#define JITTER_TARGET_LATENCY_MS 30
