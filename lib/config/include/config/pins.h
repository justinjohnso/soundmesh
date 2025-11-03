#pragma once

// I2C pins for SSD1306 display
#define I2C_MASTER_SCL_IO      6
#define I2C_MASTER_SDA_IO      5
#define I2C_MASTER_FREQ_HZ     400000
#define I2C_MASTER_NUM         I2C_NUM_0

// SSD1306 OLED (128x32 display)
#define DISPLAY_I2C_ADDR       0x3C
#define DISPLAY_WIDTH          128
#define DISPLAY_HEIGHT         32

// Button GPIO (avoid GPIO 1 = UART TX)
#define BUTTON_GPIO            GPIO_NUM_4

// I2S pins for UDA1334 DAC (RX only)
#define I2S_BCK_IO             GPIO_NUM_7
#define I2S_WS_IO              GPIO_NUM_8
#define I2S_DO_IO              GPIO_NUM_9
#define I2S_PORT               I2S_NUM_0

// ADC pins for stereo AUX input (TX only)
// A0 = GPIO1, A1 = GPIO2 on XIAO ESP32-S3
#define ADC_LEFT_GPIO          GPIO_NUM_1
#define ADC_RIGHT_GPIO         GPIO_NUM_2
#define ADC_LEFT_CHANNEL       ADC_CHANNEL_0  // GPIO1 = ADC1_CH0
#define ADC_RIGHT_CHANNEL      ADC_CHANNEL_1  // GPIO2 = ADC1_CH1
