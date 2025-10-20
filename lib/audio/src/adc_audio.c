#include "audio/adc_audio.h"

#include <esp_log.h>
#include <driver/i2c.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "config/pins.h"

#define PCF8591_ADDR 0x48

static const char *TAG = "adc_audio";

esp_err_t adc_audio_init(void) {
    // I2C is already initialized by display_init(), so we just log success
    ESP_LOGI(TAG, "ADC audio init success (I2C shared with display)");
    return ESP_OK;
}

esp_err_t adc_audio_read(uint8_t *left_value, uint8_t *right_value) {
    if (!left_value || !right_value) {
        return ESP_ERR_INVALID_ARG;
    }

    // Configure PCF8591 to read from channel 0, single ended
    uint8_t control_byte = 0x40; // channel 0, single ended

    // Write control byte for left channel (AIN0)
    esp_err_t ret = i2c_master_write_to_device(I2C_MASTER_NUM, PCF8591_ADDR, &control_byte, 1, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C write failed for left channel: %s", esp_err_to_name(ret));
        return ret;
    }

    // Small delay for ADC conversion
    vTaskDelay(pdMS_TO_TICKS(1));

    // Read left channel value
    ret = i2c_master_read_from_device(I2C_MASTER_NUM, PCF8591_ADDR, left_value, 1, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C read failed for left channel: %s", esp_err_to_name(ret));
        return ret;
    }

    // Configure PCF8591 to read from channel 1, single ended
    control_byte = 0x41; // channel 1, single ended

    // Write control byte for right channel (AIN1)
    ret = i2c_master_write_to_device(I2C_MASTER_NUM, PCF8591_ADDR, &control_byte, 1, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C write failed for right channel: %s", esp_err_to_name(ret));
        return ret;
    }

    // Small delay for ADC conversion
    vTaskDelay(pdMS_TO_TICKS(1));

    // Read right channel value
    ret = i2c_master_read_from_device(I2C_MASTER_NUM, PCF8591_ADDR, right_value, 1, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C read failed for right channel: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}
