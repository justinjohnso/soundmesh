#include "control/buttons.h"
#include "config/pins.h"
#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "buttons";
static uint32_t press_start_tick = 0;
static bool button_pressed = false;
static bool long_press_triggered = false;

#define LONG_PRESS_THRESHOLD_MS 1000

esp_err_t buttons_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_LOGI(TAG, "Button initialized on GPIO%d", BUTTON_GPIO);
    return ESP_OK;
}

button_event_t buttons_poll(void) {
    int gpio_level = gpio_get_level(BUTTON_GPIO);
    bool current_state = (gpio_level == 0);
    
    static uint32_t last_log = 0;
    uint32_t now = xTaskGetTickCount();
    if ((now - last_log) > pdMS_TO_TICKS(5000)) {
        ESP_LOGI(TAG, "Button GPIO=%d, level=%d, pressed=%d", BUTTON_GPIO, gpio_level, button_pressed);
        last_log = now;
    }

    if (current_state && !button_pressed) {
        press_start_tick = xTaskGetTickCount();
        button_pressed = true;
        long_press_triggered = false;
        ESP_LOGI(TAG, "Button pressed");
    } else if (current_state && button_pressed) {
        // Button still held
        uint32_t press_duration = (xTaskGetTickCount() - press_start_tick) * portTICK_PERIOD_MS;
        if (press_duration >= LONG_PRESS_THRESHOLD_MS && !long_press_triggered) {
            long_press_triggered = true;
            ESP_LOGI(TAG, "Button long press detected");
            return BUTTON_EVENT_LONG_PRESS;
        }
    } else if (!current_state && button_pressed) {
        // Button released
        button_pressed = false;
        uint32_t press_duration = (xTaskGetTickCount() - press_start_tick) * portTICK_PERIOD_MS;

        if (long_press_triggered) {
            // Long press was already handled
            long_press_triggered = false;
            ESP_LOGI(TAG, "Button released (long press already handled)");
            return BUTTON_EVENT_NONE;
        } else if (press_duration >= LONG_PRESS_THRESHOLD_MS) {
            ESP_LOGI(TAG, "Button long press on release");
            return BUTTON_EVENT_LONG_PRESS;
        } else {
            ESP_LOGI(TAG, "Button short press");
            return BUTTON_EVENT_SHORT_PRESS;
        }
    }

    return BUTTON_EVENT_NONE;
}
