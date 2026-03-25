#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_err.h>

#define MEMORY_MONITOR_MAX_TASKS 8

/**
 * Initialize the memory monitor module.
 * Must be called before any other memory_monitor functions.
 */
esp_err_t memory_monitor_init(void);

/**
 * Register a task for stack monitoring.
 * @param task    Task handle from xTaskCreate
 * @param name    Human-readable name (max 15 chars, will be truncated)
 * @return ESP_OK on success, ESP_ERR_NO_MEM if max tasks reached
 */
esp_err_t memory_monitor_register_task(TaskHandle_t task, const char *name);

/**
 * Get stack high water mark (minimum free stack) for a registered task.
 * @param task_name  Name used when registering
 * @return High water mark in bytes, or 0 if task not found
 */
size_t memory_monitor_get_stack_hwm(const char *task_name);

/**
 * Log stack high water marks for all registered tasks.
 */
void memory_monitor_log_all_stacks(void);

/**
 * Check if all registered tasks have sufficient stack headroom.
 * @param min_headroom_bytes  Minimum required free stack bytes
 * @return true if all tasks have at least min_headroom_bytes free
 */
bool memory_monitor_check_stack_headroom(size_t min_headroom_bytes);

/**
 * Get current free heap size in bytes.
 */
size_t memory_monitor_get_free_heap(void);

/**
 * Get largest contiguous free block in heap.
 */
size_t memory_monitor_get_largest_free_block(void);

/**
 * Calculate heap fragmentation percentage.
 * @return Fragmentation as percentage (0-100), where 0 = no fragmentation
 */
uint8_t memory_monitor_get_heap_fragmentation_pct(void);

/**
 * Log detailed heap statistics.
 */
void memory_monitor_log_heap_stats(void);

/**
 * Check if heap is healthy (sufficient free memory and unfragmented).
 * Uses MIN_FREE_HEAP_BYTES and PORTAL_MIN_LARGEST_BLOCK from build.h.
 * @return true if heap is healthy
 */
bool memory_monitor_is_heap_healthy(void);

/**
 * Start periodic memory monitoring (logs every interval_ms).
 * Creates a background task that periodically logs memory stats.
 * @param interval_ms  Logging interval in milliseconds (min 1000)
 * @return ESP_OK on success, ESP_FAIL if already running
 */
esp_err_t memory_monitor_start_periodic(uint32_t interval_ms);

/**
 * Stop periodic memory monitoring.
 */
void memory_monitor_stop_periodic(void);

/**
 * Get a snapshot of all memory stats for external use.
 */
typedef struct {
    size_t free_heap;
    size_t largest_block;
    uint8_t fragmentation_pct;
    size_t min_ever_free_heap;
    uint8_t tasks_below_headroom;
} memory_stats_t;

void memory_monitor_get_stats(memory_stats_t *stats);
