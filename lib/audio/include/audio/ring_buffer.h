#pragma once

#include <esp_err.h>
#include <stdint.h>
#include <stddef.h>

typedef struct ring_buffer_t ring_buffer_t;

ring_buffer_t* ring_buffer_create(size_t size);
void ring_buffer_destroy(ring_buffer_t *rb);
esp_err_t ring_buffer_write(ring_buffer_t *rb, const uint8_t *data, size_t len);
esp_err_t ring_buffer_read(ring_buffer_t *rb, uint8_t *data, size_t len);
size_t ring_buffer_available(ring_buffer_t *rb);
