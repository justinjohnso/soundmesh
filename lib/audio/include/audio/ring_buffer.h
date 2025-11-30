#pragma once

#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdint.h>
#include <stddef.h>

typedef struct ring_buffer_t ring_buffer_t;

/**
 * Create a byte-stream ring buffer (BYTEBUF mode).
 * Use ring_buffer_read/write for continuous stream data like PCM.
 */
ring_buffer_t* ring_buffer_create(size_t size);

/**
 * Create a ring buffer with explicit mode.
 * @param item_mode true for NOSPLIT (discrete items), false for BYTEBUF (stream)
 * 
 * NOSPLIT: Each write creates a discrete item. Receive gets exactly what was sent.
 *          Use for variable-length packets (e.g., Opus frames with length prefix).
 * BYTEBUF: Stream-based. Reads can span multiple writes.
 *          Use for continuous data like PCM audio.
 */
ring_buffer_t* ring_buffer_create_ex(size_t size, bool item_mode);

void ring_buffer_destroy(ring_buffer_t *rb);

/**
 * Set the consumer task that will be notified when data is written.
 * Enables event-driven consumption instead of polling.
 */
void ring_buffer_set_consumer(ring_buffer_t *rb, TaskHandle_t consumer);

/**
 * Write data to the ring buffer.
 * If a consumer task is set, it will be notified via xTaskNotifyGive().
 */
esp_err_t ring_buffer_write(ring_buffer_t *rb, const uint8_t *data, size_t len);

/**
 * Non-blocking read from ring buffer.
 * Returns ESP_ERR_NOT_FOUND if no data available.
 */
esp_err_t ring_buffer_read(ring_buffer_t *rb, uint8_t *data, size_t len);

/**
 * Blocking read - waits for notification from producer.
 * Use with ring_buffer_set_consumer() for event-driven pipeline.
 * Returns ESP_ERR_TIMEOUT if timeout expires before data available.
 */
esp_err_t ring_buffer_read_blocking(ring_buffer_t *rb, uint8_t *data, size_t len, TickType_t timeout);

/**
 * Get number of bytes available to read.
 */
size_t ring_buffer_available(ring_buffer_t *rb);

/**
 * Receive a complete item from the ring buffer (for variable-length packets).
 * Returns pointer to item data (must call ring_buffer_return_item when done).
 * Returns NULL if no item available.
 */
uint8_t* ring_buffer_receive_item(ring_buffer_t *rb, size_t *item_size);

/**
 * Return an item received via ring_buffer_receive_item().
 */
void ring_buffer_return_item(ring_buffer_t *rb, uint8_t *item);
