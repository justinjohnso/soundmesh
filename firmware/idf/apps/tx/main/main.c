#include "audio_pipeline.h"
#include "audio_element.h"
#include "i2s_stream.h"
#include "usb_stream.h"
#include "opus_encoder.h"
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
    audio_element_handle_t src_usb = NULL; // usb_stream as default; TODO: allow i2s line-in
    audio_element_handle_t enc = NULL;
    audio_element_handle_t mesh = NULL;

    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);

    // USB audio in
    usb_stream_cfg_t usb_cfg = {0};
    usb_cfg.stream_type = AUDIO_STREAM_READER;
    src_usb = usb_stream_init(&usb_cfg);

    // Opus encoder
    opus_encoder_cfg_t opus_cfg = DEFAULT_OPUS_ENCODER_CONFIG();
    opus_cfg.sample_rate = 16000;
    opus_cfg.channels = 1;
    opus_cfg.frame_ms = 10;
    enc = encoder_opus_init(&opus_cfg);

    // Mesh writer
    mesh_stream_cfg_t mcfg = {
        .is_writer = 1,
        .jitter_ms = 0,
        .group_broadcast = 1,
        .rx_queue_len = 32,
    };
    mesh = mesh_stream_init(&mcfg);

    audio_pipeline_register(pipeline, src_usb, "usb");
    audio_pipeline_register(pipeline, enc, "enc");
    audio_pipeline_register(pipeline, mesh, "mesh");

    const char *link_tag[3] = {"usb", "enc", "mesh"};
    audio_pipeline_link(pipeline, &link_tag[0], 3);

    audio_pipeline_run(pipeline);

    ESP_LOGI(TAG, "Transmitter running (USB->Opus->Mesh)");
}
