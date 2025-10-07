#include "audio_pipeline.h"
#include "audio_element.h"
#include "i2s_stream.h"
#include "usb_stream.h"
#include "opus_encoder.h"
#include "opus_decoder.h"
#include "mesh_stream.h"
#include "ctrl_plane.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"

static const char *TAG = "app_node";

#define GPIO_MODE_SWITCH GPIO_NUM_0 // TODO: set actual pin for physical TX/RX switch

static void build_tx(audio_pipeline_handle_t *pl) {
    audio_pipeline_handle_t pipeline;
    audio_element_handle_t src_usb, enc, mesh;
    audio_pipeline_cfg_t cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&cfg);

    usb_stream_cfg_t usb_cfg = { .stream_type = AUDIO_STREAM_READER };
    src_usb = usb_stream_init(&usb_cfg);

    opus_encoder_cfg_t opus_cfg = DEFAULT_OPUS_ENCODER_CONFIG();
    opus_cfg.sample_rate = 16000; opus_cfg.channels = 1; opus_cfg.frame_ms = 10;
    enc = encoder_opus_init(&opus_cfg);

    mesh_stream_cfg_t mcfg = { .is_writer = 1, .jitter_ms = 0, .group_broadcast = 1, .rx_queue_len = 32 };
    audio_element_handle_t mesh_el = mesh_stream_init(&mcfg);

    audio_pipeline_register(pipeline, src_usb, "usb");
    audio_pipeline_register(pipeline, enc, "enc");
    audio_pipeline_register(pipeline, mesh_el, "mesh");
    const char *links[3] = {"usb", "enc", "mesh"};
    audio_pipeline_link(pipeline, links, 3);
    *pl = pipeline;
}

static void build_rx(audio_pipeline_handle_t *pl) {
    audio_pipeline_handle_t pipeline;
    audio_element_handle_t mesh, dec, sink_usb;
    audio_pipeline_cfg_t cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&cfg);

    mesh_stream_cfg_t mcfg = { .is_writer = 0, .jitter_ms = 80, .group_broadcast = 1, .rx_queue_len = 64 };
    mesh = mesh_stream_init(&mcfg);

    opus_decoder_cfg_t opus_cfg = DEFAULT_OPUS_DECODER_CONFIG();
    dec = decoder_opus_init(&opus_cfg);

    usb_stream_cfg_t usb_cfg = { .stream_type = AUDIO_STREAM_WRITER };
    sink_usb = usb_stream_init(&usb_cfg);

    audio_pipeline_register(pipeline, mesh, "mesh");
    audio_pipeline_register(pipeline, dec, "dec");
    audio_pipeline_register(pipeline, sink_usb, "usb");
    const char *links[3] = {"mesh", "dec", "usb"};
    audio_pipeline_link(pipeline, links, 3);
    *pl = pipeline;
}

void app_main(void) {
    esp_event_loop_create_default();
    esp_netif_init();
    // Initialize WiFi + Mesh here (stub)

    gpio_config_t io = { .pin_bit_mask = 1ULL << GPIO_MODE_SWITCH, .mode = GPIO_MODE_INPUT, .pull_up_en = 1 };
    gpio_config(&io);

    int is_tx = gpio_get_level(GPIO_MODE_SWITCH) == 1;
    audio_pipeline_handle_t pipeline = NULL;
    if (is_tx) build_tx(&pipeline); else build_rx(&pipeline);

    audio_pipeline_run(pipeline);
    ESP_LOGI(TAG, "Node running in %s mode", is_tx ? "TX" : "RX");
}
