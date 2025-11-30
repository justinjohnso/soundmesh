#include "audio/ring_buffer.h"
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <string.h>

static const char *TAG = "ring_buffer";

struct ring_buffer_t {
    RingbufHandle_t handle;
    TaskHandle_t consumer;  // Task to notify on write (event-driven)
};

ring_buffer_t* ring_buffer_create(size_t size) {
    return ring_buffer_create_ex(size, false);
}

ring_buffer_t* ring_buffer_create_ex(size_t size, bool item_mode) {
    ring_buffer_t *rb = malloc(sizeof(ring_buffer_t));
    if (!rb) return NULL;
    
    RingbufferType_t type = item_mode ? RINGBUF_TYPE_NOSPLIT : RINGBUF_TYPE_BYTEBUF;
    rb->handle = xRingbufferCreate(size, type);
    if (!rb->handle) {
        free(rb);
        return NULL;
    }
    
    rb->consumer = NULL;
    
    ESP_LOGI(TAG, "Ring buffer created: %u bytes, mode=%s", size, 
             item_mode ? "ITEM" : "BYTE");
    return rb;
}

void ring_buffer_destroy(ring_buffer_t *rb) {
    if (rb) {
        if (rb->handle) {
            vRingbufferDelete(rb->handle);
        }
        free(rb);
    }
}

void ring_buffer_set_consumer(ring_buffer_t *rb, TaskHandle_t consumer) {
    if (rb) {
        rb->consumer = consumer;
        ESP_LOGI(TAG, "Consumer task set: %p", consumer);
    }
}

esp_err_t ring_buffer_write(ring_buffer_t *rb, const uint8_t *data, size_t len) {
    if (!rb || !rb->handle) return ESP_ERR_INVALID_ARG;
    
    if (xRingbufferSend(rb->handle, data, len, pdMS_TO_TICKS(10)) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    
    // Notify consumer task that data is available (event-driven)
    if (rb->consumer) {
        xTaskNotifyGive(rb->consumer);
    }
    
    return ESP_OK;
}

esp_err_t ring_buffer_read(ring_buffer_t *rb, uint8_t *data, size_t len) {
    if (!rb || !rb->handle) return ESP_ERR_INVALID_ARG;
    
    size_t item_size = 0;
    // CRITICAL: Use xRingbufferReceiveUpTo for BYTEBUF to avoid discarding extra bytes!
    // xRingbufferReceive returns ALL contiguous bytes, but ReturnItem frees them ALL.
    // ReceiveUpTo limits to exactly `len` bytes, preserving remaining data.
    uint8_t *item = xRingbufferReceiveUpTo(rb->handle, &item_size, 0, len);
    
    if (!item) {
        return ESP_ERR_NOT_FOUND;
    }
    
    memcpy(data, item, item_size);
    vRingbufferReturnItem(rb->handle, item);
    
    return ESP_OK;
}

esp_err_t ring_buffer_read_blocking(ring_buffer_t *rb, uint8_t *data, size_t len, TickType_t timeout) {
    if (!rb || !rb->handle) return ESP_ERR_INVALID_ARG;
    
    TickType_t start = xTaskGetTickCount();
    TickType_t remaining = timeout;
    
    while (remaining > 0 || timeout == portMAX_DELAY) {
        // Wait for notification from producer
        uint32_t notified = ulTaskNotifyTake(pdTRUE, remaining);
        
        if (notified == 0 && timeout != portMAX_DELAY) {
            // Timeout expired
            return ESP_ERR_TIMEOUT;
        }
        
        // Check if enough data is available
        if (ring_buffer_available(rb) >= len) {
            size_t item_size = 0;
            uint8_t *item = xRingbufferReceiveUpTo(rb->handle, &item_size, 0, len);
            
            if (item && item_size == len) {
                memcpy(data, item, len);
                vRingbufferReturnItem(rb->handle, item);
                return ESP_OK;
            }
            
            if (item) {
                vRingbufferReturnItem(rb->handle, item);
            }
        }
        
        // Update remaining time
        if (timeout != portMAX_DELAY) {
            TickType_t elapsed = xTaskGetTickCount() - start;
            remaining = (elapsed < timeout) ? (timeout - elapsed) : 0;
        }
    }
    
    return ESP_ERR_TIMEOUT;
}

size_t ring_buffer_available(ring_buffer_t *rb) {
    if (!rb || !rb->handle) return 0;
    
    UBaseType_t waiting;
    vRingbufferGetInfo(rb->handle, NULL, NULL, NULL, NULL, &waiting);
    return waiting;
}

uint8_t* ring_buffer_receive_item(ring_buffer_t *rb, size_t *item_size) {
    if (!rb || !rb->handle || !item_size) return NULL;
    
    return (uint8_t*)xRingbufferReceive(rb->handle, item_size, 0);
}

void ring_buffer_return_item(ring_buffer_t *rb, uint8_t *item) {
    if (rb && rb->handle && item) {
        vRingbufferReturnItem(rb->handle, item);
    }
}
