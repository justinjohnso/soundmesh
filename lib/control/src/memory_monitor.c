#include "control/memory_monitor.h"
#include "config/build.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/semphr.h>
#include <string.h>

static const char *TAG = "mem_monitor";

typedef struct {
    TaskHandle_t handle;
    char name[16];
    bool active;
} registered_task_t;

static registered_task_t s_tasks[MEMORY_MONITOR_MAX_TASKS];
static size_t s_task_count = 0;
static SemaphoreHandle_t s_mutex = NULL;
static TaskHandle_t s_periodic_task = NULL;
static uint32_t s_periodic_interval_ms = 0;
static bool s_initialized = false;

esp_err_t memory_monitor_init(void) {
    if (s_initialized) {
        return ESP_OK;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    memset(s_tasks, 0, sizeof(s_tasks));
    s_task_count = 0;
    s_initialized = true;

    ESP_LOGI(TAG, "Memory monitor initialized");
    return ESP_OK;
}

esp_err_t memory_monitor_register_task(TaskHandle_t task, const char *name) {
    if (!s_initialized || !s_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!task || !name) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_task_count >= MEMORY_MONITOR_MAX_TASKS) {
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "Max tasks reached, cannot register: %s", name);
        return ESP_ERR_NO_MEM;
    }

    // Check for duplicate
    for (size_t i = 0; i < s_task_count; i++) {
        if (s_tasks[i].active && s_tasks[i].handle == task) {
            xSemaphoreGive(s_mutex);
            ESP_LOGW(TAG, "Task already registered: %s", name);
            return ESP_ERR_INVALID_STATE;
        }
    }

    registered_task_t *slot = &s_tasks[s_task_count];
    slot->handle = task;
    strncpy(slot->name, name, sizeof(slot->name) - 1);
    slot->name[sizeof(slot->name) - 1] = '\0';
    slot->active = true;
    s_task_count++;

    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Registered task: %s", name);
    return ESP_OK;
}

size_t memory_monitor_get_stack_hwm(const char *task_name) {
    if (!s_initialized || !s_mutex || !task_name) {
        return 0;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    for (size_t i = 0; i < s_task_count; i++) {
        if (s_tasks[i].active && strcmp(s_tasks[i].name, task_name) == 0) {
            UBaseType_t hwm_words = uxTaskGetStackHighWaterMark(s_tasks[i].handle);
            xSemaphoreGive(s_mutex);
            return hwm_words * sizeof(StackType_t);
        }
    }

    xSemaphoreGive(s_mutex);
    return 0;
}

void memory_monitor_log_all_stacks(void) {
    if (!s_initialized || !s_mutex) {
        ESP_LOGW(TAG, "Not initialized");
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    ESP_LOGI(TAG, "=== Stack High Water Marks ===");
    for (size_t i = 0; i < s_task_count; i++) {
        if (s_tasks[i].active) {
            UBaseType_t hwm_words = uxTaskGetStackHighWaterMark(s_tasks[i].handle);
            size_t hwm_bytes = hwm_words * sizeof(StackType_t);
            const char *status = (hwm_bytes < MIN_STACK_HEADROOM_BYTES) ? " [LOW!]" : "";
            ESP_LOGI(TAG, "  %-12s: %5u bytes free%s", s_tasks[i].name, hwm_bytes, status);
        }
    }

    xSemaphoreGive(s_mutex);
}

bool memory_monitor_check_stack_headroom(size_t min_headroom_bytes) {
    if (!s_initialized || !s_mutex) {
        return false;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    bool all_ok = true;
    for (size_t i = 0; i < s_task_count; i++) {
        if (s_tasks[i].active) {
            UBaseType_t hwm_words = uxTaskGetStackHighWaterMark(s_tasks[i].handle);
            size_t hwm_bytes = hwm_words * sizeof(StackType_t);
            if (hwm_bytes < min_headroom_bytes) {
                ESP_LOGW(TAG, "Low stack headroom: %s (%u bytes < %u)",
                         s_tasks[i].name, hwm_bytes, min_headroom_bytes);
                all_ok = false;
            }
        }
    }

    xSemaphoreGive(s_mutex);
    return all_ok;
}

size_t memory_monitor_get_free_heap(void) {
    return heap_caps_get_free_size(MALLOC_CAP_8BIT);
}

size_t memory_monitor_get_largest_free_block(void) {
    return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
}

uint8_t memory_monitor_get_heap_fragmentation_pct(void) {
    size_t free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

    if (free == 0) {
        return 100;
    }
    if (largest >= free) {
        return 0;
    }

    // Fragmentation = 1 - (largest_block / total_free)
    // As percentage: 100 * (1 - largest/free) = 100 - (100 * largest / free)
    uint8_t frag = 100 - (uint8_t)((100 * largest) / free);
    return frag;
}

void memory_monitor_log_heap_stats(void) {
    size_t free = memory_monitor_get_free_heap();
    size_t largest = memory_monitor_get_largest_free_block();
    uint8_t frag = memory_monitor_get_heap_fragmentation_pct();
    size_t min_ever = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);

    ESP_LOGI(TAG, "=== Heap Statistics ===");
    ESP_LOGI(TAG, "  Free heap:       %6u bytes", free);
    ESP_LOGI(TAG, "  Largest block:   %6u bytes", largest);
    ESP_LOGI(TAG, "  Fragmentation:   %3u%%", frag);
    ESP_LOGI(TAG, "  Min ever free:   %6u bytes", min_ever);

    if (free < MIN_FREE_HEAP_BYTES) {
        ESP_LOGW(TAG, "  WARNING: Free heap below threshold (%u < %u)",
                 free, MIN_FREE_HEAP_BYTES);
    }
}

bool memory_monitor_is_heap_healthy(void) {
    size_t free = memory_monitor_get_free_heap();
    size_t largest = memory_monitor_get_largest_free_block();

    bool heap_ok = (free >= MIN_FREE_HEAP_BYTES);
    bool block_ok = (largest >= PORTAL_MIN_LARGEST_BLOCK);

    if (!heap_ok) {
        ESP_LOGW(TAG, "Heap low: %u < %u", free, MIN_FREE_HEAP_BYTES);
    }
    if (!block_ok) {
        ESP_LOGW(TAG, "Fragmented: largest block %u < %u", largest, PORTAL_MIN_LARGEST_BLOCK);
    }

    return heap_ok && block_ok;
}

static void periodic_monitor_task(void *arg) {
    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(s_periodic_interval_ms));

        memory_monitor_log_heap_stats();
        memory_monitor_log_all_stacks();

        // Check thresholds and warn
        if (!memory_monitor_is_heap_healthy()) {
            ESP_LOGW(TAG, "Heap health check failed!");
        }
        if (!memory_monitor_check_stack_headroom(MIN_STACK_HEADROOM_BYTES)) {
            ESP_LOGW(TAG, "Some tasks have low stack headroom!");
        }
    }
}

esp_err_t memory_monitor_start_periodic(uint32_t interval_ms) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_periodic_task) {
        ESP_LOGW(TAG, "Periodic monitoring already running");
        return ESP_FAIL;
    }
    if (interval_ms < 1000) {
        interval_ms = 1000;
    }

    s_periodic_interval_ms = interval_ms;

    BaseType_t ret = xTaskCreate(
        periodic_monitor_task,
        "mem_mon",
        MEMORY_MONITOR_TASK_STACK,
        NULL,
        MEMORY_MONITOR_TASK_PRIO,
        &s_periodic_task
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create periodic task");
        s_periodic_task = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Started periodic monitoring (interval=%lums)", (unsigned long)interval_ms);
    return ESP_OK;
}

void memory_monitor_stop_periodic(void) {
    if (s_periodic_task) {
        vTaskDelete(s_periodic_task);
        s_periodic_task = NULL;
        ESP_LOGI(TAG, "Stopped periodic monitoring");
    }
}

void memory_monitor_get_stats(memory_stats_t *stats) {
    if (!stats) {
        return;
    }

    stats->free_heap = memory_monitor_get_free_heap();
    stats->largest_block = memory_monitor_get_largest_free_block();
    stats->fragmentation_pct = memory_monitor_get_heap_fragmentation_pct();
    stats->min_ever_free_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);

    // Count tasks below headroom threshold
    stats->tasks_below_headroom = 0;
    if (s_initialized && s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        for (size_t i = 0; i < s_task_count; i++) {
            if (s_tasks[i].active) {
                UBaseType_t hwm = uxTaskGetStackHighWaterMark(s_tasks[i].handle);
                size_t hwm_bytes = hwm * sizeof(StackType_t);
                if (hwm_bytes < MIN_STACK_HEADROOM_BYTES) {
                    stats->tasks_below_headroom++;
                }
            }
        }
        xSemaphoreGive(s_mutex);
    }
}
