#include "audio_pipeline.h"
#include "i2s_stream.h"
#include "raw_stream.h"
#include "mesh_stream.h"
#include "ctrl_plane.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"

static const char *TAG = "app_tx";

void app_main(void) {
    esp_event_loop_create_default();
    esp_netif_init();
    // Initialize WiFi + Mesh here (stub)

    audio_pipeline_handle_t pipeline;
    audio_element_handle_t tone = NULL; // placeholder source via raw_stream/tone
    audio_element_handle_t mesh = NULL;

    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);

    // Simple raw source placeholder (we can later swap for I2S/USB)
    raw_stream_cfg_t raw_cfg = {
        .type = AUDIO_STREAM_READER,
    };
    tone = raw_stream_init(&raw_cfg);

    // Mesh writer
    mesh_stream_cfg_t mcfg = {
        .is_writer = 1,
        .jitter_ms = 0,
        .group_broadcast = 1,
        .rx_queue_len = 32,
    };
    mesh = mesh_stream_init(&mcfg);

    audio_pipeline_register(pipeline, tone, "src");
    audio_pipeline_register(pipeline, mesh, "mesh");

    const char *link_tag[2] = {"src", "mesh"};
    audio_pipeline_link(pipeline, &link_tag[0], 2);

    audio_pipeline_run(pipeline);

    ESP_LOGI(TAG, "Transmitter running (Raw->Mesh placeholder)");
}
