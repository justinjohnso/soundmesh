#include "audio/pipeline.h"
#include "common/config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "jitter_buffer";

struct jitter_buffer_s {
    RingbufHandle_t ring_buffer;
    uint16_t buffer_packets;
    uint16_t target_latency_ms;
    uint32_t underruns;
    uint32_t overruns;
};

esp_err_t jitter_buffer_init(const jitter_buffer_config_t *config, jitter_buffer_handle_t *out_handle) {
    if (!config || !out_handle) {
        return ESP_ERR_INVALID_ARG;
    }

    jitter_buffer_handle_t handle = malloc(sizeof(struct jitter_buffer_s));
    if (!handle) {
        return ESP_ERR_NO_MEM;
    }

    size_t buffer_size = config->buffer_packets * AUDIO_SAMPLES_PER_PACKET * sizeof(int16_t);
    handle->ring_buffer = xRingbufferCreate(buffer_size, RINGBUF_TYPE_BYTEBUF);
    if (!handle->ring_buffer) {
        free(handle);
        return ESP_ERR_NO_MEM;
    }

    handle->buffer_packets = config->buffer_packets;
    handle->target_latency_ms = config->target_latency_ms;
    handle->underruns = 0;
    handle->overruns = 0;

    *out_handle = handle;
    ESP_LOGI(TAG, "Jitter buffer initialized (%d packets, target %d ms)",
            config->buffer_packets, config->target_latency_ms);
    return ESP_OK;
}

esp_err_t jitter_buffer_push(jitter_buffer_handle_t handle, const int16_t *pcm, uint16_t num_samples) {
    if (!handle || !pcm) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t bytes = num_samples * sizeof(int16_t);
    BaseType_t ret = xRingbufferSend(handle->ring_buffer, pcm, bytes, 0);
    
    if (ret != pdTRUE) {
        handle->overruns++;
        ESP_LOGW(TAG, "Jitter buffer overrun (total: %ld)", handle->overruns);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t jitter_buffer_pop(jitter_buffer_handle_t handle, int16_t *out_pcm, uint16_t num_samples) {
    if (!handle || !out_pcm) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t item_size = 0;
    void *item = xRingbufferReceive(handle->ring_buffer, &item_size, pdMS_TO_TICKS(20));
    
    if (!item) {
        handle->underruns++;
        ESP_LOGW(TAG, "Jitter buffer underrun (total: %ld)", handle->underruns);
        memset(out_pcm, 0, num_samples * sizeof(int16_t));
        return ESP_ERR_TIMEOUT;
    }

    size_t copy_size = (item_size < num_samples * sizeof(int16_t)) ? 
                      item_size : num_samples * sizeof(int16_t);
    memcpy(out_pcm, item, copy_size);
    vRingbufferReturnItem(handle->ring_buffer, item);

    return ESP_OK;
}

uint32_t jitter_buffer_get_fill_level(jitter_buffer_handle_t handle) {
    if (!handle) {
        return 0;
    }

    UBaseType_t items_waiting = 0;
    vRingbufferGetInfo(handle->ring_buffer, NULL, NULL, NULL, NULL, &items_waiting);
    return items_waiting;
}

void jitter_buffer_deinit(jitter_buffer_handle_t handle) {
    if (handle) {
        ESP_LOGI(TAG, "Jitter buffer stats: underruns=%ld, overruns=%ld",
                handle->underruns, handle->overruns);
        vRingbufferDelete(handle->ring_buffer);
        free(handle);
    }
}
