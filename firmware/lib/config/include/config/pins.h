#pragma once

// I2C pins for SSD1306 display
#define I2C_MASTER_SCL_IO      6
#define I2C_MASTER_SDA_IO      5
#define I2C_MASTER_FREQ_HZ     400000
#define I2C_MASTER_NUM         I2C_NUM_0

// SSD1306 OLED
#define DISPLAY_I2C_ADDR       0x3C
#define DISPLAY_WIDTH          128
#define DISPLAY_HEIGHT         64

// Button GPIO
#define BUTTON_GPIO            GPIO_NUM_1

// I2S pins for UDA1334 DAC (RX only)
#define I2S_BCK_IO             GPIO_NUM_7
#define I2S_WS_IO              GPIO_NUM_8
#define I2S_DO_IO              GPIO_NUM_9
#define I2S_PORT               I2S_NUM_0
