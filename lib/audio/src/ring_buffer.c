#include "audio/ring_buffer.h"
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <esp_log.h>
#include <string.h>

static const char *TAG = "ring_buffer";

struct ring_buffer_t {
    RingbufHandle_t handle;
};

ring_buffer_t* ring_buffer_create(size_t size) {
    ring_buffer_t *rb = malloc(sizeof(ring_buffer_t));
    if (!rb) return NULL;
    
    rb->handle = xRingbufferCreate(size, RINGBUF_TYPE_BYTEBUF);
    if (!rb->handle) {
        free(rb);
        return NULL;
    }
    
    ESP_LOGI(TAG, "Ring buffer created: %u bytes", size);
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

esp_err_t ring_buffer_write(ring_buffer_t *rb, const uint8_t *data, size_t len) {
    if (!rb || !rb->handle) return ESP_ERR_INVALID_ARG;
    
    if (xRingbufferSend(rb->handle, data, len, pdMS_TO_TICKS(10)) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    
    return ESP_OK;
}

esp_err_t ring_buffer_read(ring_buffer_t *rb, uint8_t *data, size_t len) {
    if (!rb || !rb->handle) return ESP_ERR_INVALID_ARG;
    
    size_t item_size;
    uint8_t *item = xRingbufferReceive(rb->handle, &item_size, pdMS_TO_TICKS(10));
    
    if (!item) {
        return ESP_ERR_NOT_FOUND;
    }
    
    size_t copy_len = (item_size < len) ? item_size : len;
    memcpy(data, item, copy_len);
    vRingbufferReturnItem(rb->handle, item);
    
    return ESP_OK;
}

size_t ring_buffer_available(ring_buffer_t *rb) {
    if (!rb || !rb->handle) return 0;
    
    UBaseType_t waiting;
    vRingbufferGetInfo(rb->handle, NULL, NULL, NULL, NULL, &waiting);
    return waiting;
}
