#include "control/buttons.h"
#include "config/pins.h"
#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "buttons";
static uint32_t press_start_tick = 0;
static bool button_pressed = false;

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
    bool current_state = gpio_get_level(BUTTON_GPIO) == 0;
    
    if (current_state && !button_pressed) {
        press_start_tick = xTaskGetTickCount();
        button_pressed = true;
    } else if (!current_state && button_pressed) {
        uint32_t press_duration = (xTaskGetTickCount() - press_start_tick) * portTICK_PERIOD_MS;
        button_pressed = false;
        
        if (press_duration >= LONG_PRESS_THRESHOLD_MS) {
            return BUTTON_EVENT_LONG_PRESS;
        } else {
            return BUTTON_EVENT_SHORT_PRESS;
        }
    }
    
    return BUTTON_EVENT_NONE;
}
