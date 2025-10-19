#include "control/button.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

static const char *TAG = "button";

struct button_handle_s {
    int gpio_num;
    uint32_t debounce_ms;
    uint32_t long_press_ms;
    QueueHandle_t event_queue;
    TaskHandle_t task_handle;
    bool task_running;
};

static void button_task(void *arg) {
    button_handle_t handle = (button_handle_t)arg;
    bool last_state = true;  // Pull-up, so idle is high
    uint32_t press_start_time = 0;
    bool press_handled = false;

    while (handle->task_running) {
        bool current_state = gpio_get_level(handle->gpio_num);

        if (!current_state && last_state) {
            press_start_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
            press_handled = false;
            ESP_LOGD(TAG, "Button pressed");
        }
        else if (current_state && !last_state) {
            uint32_t press_duration = (xTaskGetTickCount() * portTICK_PERIOD_MS) - press_start_time;
            
            if (!press_handled) {
                button_event_t event;
                if (press_duration >= handle->long_press_ms) {
                    event = BUTTON_EVENT_LONG_PRESS;
                    ESP_LOGI(TAG, "Long press detected (%ld ms)", press_duration);
                } else if (press_duration >= handle->debounce_ms) {
                    event = BUTTON_EVENT_SHORT_PRESS;
                    ESP_LOGI(TAG, "Short press detected (%ld ms)", press_duration);
                } else {
                    event = BUTTON_EVENT_NONE;
                }

                if (event != BUTTON_EVENT_NONE) {
                    xQueueSend(handle->event_queue, &event, 0);
                }
            }
        }
        else if (!current_state && !last_state) {
            uint32_t press_duration = (xTaskGetTickCount() * portTICK_PERIOD_MS) - press_start_time;
            
            if (!press_handled && press_duration >= handle->long_press_ms) {
                button_event_t event = BUTTON_EVENT_LONG_PRESS;
                xQueueSend(handle->event_queue, &event, 0);
                press_handled = true;
                ESP_LOGI(TAG, "Long press triggered while held");
            }
        }

        last_state = current_state;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    vTaskDelete(NULL);
}

esp_err_t button_init(const button_config_t *config, button_handle_t *out_handle) {
    if (!config || !out_handle) {
        return ESP_ERR_INVALID_ARG;
    }

    button_handle_t handle = malloc(sizeof(struct button_handle_s));
    if (!handle) {
        return ESP_ERR_NO_MEM;
    }

    handle->gpio_num = config->gpio_num;
    handle->debounce_ms = config->debounce_ms;
    handle->long_press_ms = config->long_press_ms;
    handle->task_running = true;

    handle->event_queue = xQueueCreate(4, sizeof(button_event_t));
    if (!handle->event_queue) {
        free(handle);
        return ESP_ERR_NO_MEM;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << config->gpio_num),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        vQueueDelete(handle->event_queue);
        free(handle);
        return ret;
    }

    BaseType_t task_ret = xTaskCreate(button_task, "button_task", 2048, handle, 5, &handle->task_handle);
    if (task_ret != pdPASS) {
        vQueueDelete(handle->event_queue);
        free(handle);
        return ESP_FAIL;
    }

    *out_handle = handle;
    ESP_LOGI(TAG, "Button initialized on GPIO%d", config->gpio_num);
    return ESP_OK;
}

button_event_t button_get_event(button_handle_t handle, uint32_t timeout_ms) {
    if (!handle) {
        return BUTTON_EVENT_NONE;
    }

    button_event_t event = BUTTON_EVENT_NONE;
    xQueueReceive(handle->event_queue, &event, pdMS_TO_TICKS(timeout_ms));
    return event;
}

void button_deinit(button_handle_t handle) {
    if (handle) {
        handle->task_running = false;
        vTaskDelay(pdMS_TO_TICKS(100));
        vQueueDelete(handle->event_queue);
        free(handle);
        ESP_LOGI(TAG, "Button deinitialized");
    }
}
