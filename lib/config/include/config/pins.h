#pragma once

// I2C pins for SSD1306 display and ES8388 codec (shared bus)
#define I2C_MASTER_SCL_IO      6
#define I2C_MASTER_SDA_IO      5
#define I2C_MASTER_FREQ_HZ     400000
#define I2C_MASTER_NUM         I2C_NUM_0

// SSD1306 OLED (128x32 display)
#define DISPLAY_I2C_ADDR       0x3C
#define DISPLAY_WIDTH          128
#define DISPLAY_HEIGHT         32

// Button GPIO
#define BUTTON_GPIO            GPIO_NUM_43

// ============================================================================
// I2S pins for UDA1334 DAC (RX only - receives from mesh network)
// ============================================================================
#define I2S_BCK_IO             GPIO_NUM_7
#define I2S_WS_IO              GPIO_NUM_8
#define I2S_DO_IO              GPIO_NUM_9
#define I2S_PORT               I2S_NUM_0

// ============================================================================
// ES8388 Audio Codec pins (TX/COMBO - PCBArtists ES8388 Module)
// 
// The ES8388 replaces the ADC-based audio input for TX/COMBO devices.
// It provides both line input (LIN2/RIN2) and headphone output.
//
// Wiring:
//   XIAO ESP32-S3    ES8388 Module
//   ─────────────    ─────────────
//   3.3V  ────────── DVDD (pin 16), AVDD (pin 5)
//   GND   ────────── GND (pins 3,4,6,11,14)
//   A0/GPIO1 ─────── MCLK (pin 15)
//   A1/GPIO2 ─────── DOUT (pin 20) - audio FROM codec
//   SDA/GPIO5 ────── SDA (pin 12)
//   SCL/GPIO6 ────── SCL (pin 13)
//   SCK/GPIO7 ────── SCLK (pin 17)
//   MISO/GPIO8 ───── LRCLK (pin 19)
//   MOSI/GPIO9 ───── DIN (pin 18) - audio TO codec
// ============================================================================
#define ES8388_I2C_ADDR        0x10

// I2S pins for ES8388 (shares BCLK/WS/DOUT with UDA1334 pins)
#define ES8388_MCLK_IO         GPIO_NUM_1   // A0 - Master clock to codec
#define ES8388_BCLK_IO         GPIO_NUM_7   // SCK - Bit clock (same as I2S_BCK_IO)
#define ES8388_WS_IO           GPIO_NUM_8   // MISO - Word select (same as I2S_WS_IO)
#define ES8388_DOUT_IO         GPIO_NUM_9   // MOSI - Data TO codec (same as I2S_DO_IO)
#define ES8388_DIN_IO          GPIO_NUM_2   // A1 - Data FROM codec (new pin)

// ============================================================================
// Legacy ADC pins (no longer used with ES8388, kept for reference)
// ============================================================================
// ADC pins were used for aux input before ES8388 integration
// A0 = GPIO1, A1 = GPIO2 on XIAO ESP32-S3
// Now GPIO1 = MCLK, GPIO2 = I2S_DIN for ES8388
#define ADC_LEFT_GPIO          GPIO_NUM_1
#define ADC_RIGHT_GPIO         GPIO_NUM_2
#define ADC_LEFT_CHANNEL       ADC_CHANNEL_0  // GPIO1 = ADC1_CH0
#define ADC_RIGHT_CHANNEL      ADC_CHANNEL_1  // GPIO2 = ADC1_CH1
