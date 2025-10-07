#include "audio_pipeline.h"
#include "audio_element.h"
#include "i2s_stream.h"
#include "usb_stream.h"
#include "opus_decoder.h"
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
    audio_element_handle_t dec = NULL;
    audio_element_handle_t sink_usb = NULL; // default USB out; TODO: allow I2S sink

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

    // Opus decoder
    opus_decoder_cfg_t opus_cfg = DEFAULT_OPUS_DECODER_CONFIG();
    dec = decoder_opus_init(&opus_cfg);

    // USB audio out (TODO: add Kconfig switch to select I2S sink)
    usb_stream_cfg_t usb_cfg = {0};
    usb_cfg.stream_type = AUDIO_STREAM_WRITER;
    sink_usb = usb_stream_init(&usb_cfg);

    audio_pipeline_register(pipeline, mesh, "mesh");
    audio_pipeline_register(pipeline, dec, "dec");
    audio_pipeline_register(pipeline, sink_usb, "usb");

    const char *link_tag[3] = {"mesh", "dec", "usb"};
    audio_pipeline_link(pipeline, &link_tag[0], 3);

    audio_pipeline_run(pipeline);

    ESP_LOGI(TAG, "Receiver running (Mesh->Opus->USB)");
}
