#include "audio_pipeline.h"
#include "i2s_stream.h"
#include "raw_stream.h"
#include "mesh_stream.h"
#include "ctrl_plane.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"

static const char *TAG = "app_rx";

void app_main(void) {
    esp_event_loop_create_default();
    esp_netif_init();
    // Initialize WiFi + Mesh here (stub)

    audio_pipeline_handle_t pipeline;
    audio_element_handle_t mesh = NULL;
    audio_element_handle_t sink = NULL; // raw sink placeholder

    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);

    // Mesh reader
    mesh_stream_cfg_t mcfg = {
        .is_writer = 0,
        .jitter_ms = 80,
        .group_broadcast = 1,
        .rx_queue_len = 64,
    };
    mesh = mesh_stream_init(&mcfg);

    // Raw sink (placeholder)
    raw_stream_cfg_t raw_cfg = { .type = AUDIO_STREAM_WRITER };
    sink = raw_stream_init(&raw_cfg);

    audio_pipeline_register(pipeline, mesh, "mesh");
    audio_pipeline_register(pipeline, sink, "sink");

    const char *link_tag[2] = {"mesh", "sink"};
    audio_pipeline_link(pipeline, &link_tag[0], 2);

    audio_pipeline_run(pipeline);

    ESP_LOGI(TAG, "Receiver running (Mesh->Raw placeholder)");
}
