#include "mesh/mesh_rx.h"
#include "mesh/mesh_state.h"
#include "mesh/mesh_dedupe.h"
#include "mesh/mesh_ping.h"
#include "mesh/mesh_uplink.h"
#include "network/frame_codec.h"
#include "network/mesh_net.h"
#include <esp_log.h>
#include <esp_mesh.h>
#include <string.h>

static const char *TAG = "network_mesh";

typedef struct {
    uint32_t timestamp;
    const char *src_id;
} audio_batch_callback_ctx_t;

static void on_audio_batch_frame(const uint8_t *frame,
                                 uint16_t frame_len,
                                 uint16_t frame_seq,
                                 void *ctx) {
    if (!audio_rx_callback || !ctx) {
        return;
    }
    audio_batch_callback_ctx_t *batch = (audio_batch_callback_ctx_t *)ctx;
    audio_rx_callback(frame, frame_len, frame_seq, batch->timestamp, batch->src_id);
}

void mesh_rx_task(void *arg) {
    (void)arg;
    esp_err_t err;
    mesh_addr_t from;
    mesh_data_t data;
    int flag = 0;

    ESP_LOGI(TAG, "Mesh RX task started");

    while (!is_mesh_root_ready && !is_mesh_connected) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "Mesh RX task: network ready, entering recv loop (root=%d, connected=%d)",
             is_mesh_root, is_mesh_connected);

    while (1) {
        data.data = mesh_rx_buffer;
        data.size = MESH_RX_BUFFER_SIZE;

        err = esp_mesh_recv(&from, &data, portMAX_DELAY, &flag, NULL, 0);

        if (err != ESP_OK) {
            if (err == ESP_ERR_MESH_NOT_START) {
                vTaskDelay(pdMS_TO_TICKS(100));
            } else {
                ESP_LOGW(TAG, "Mesh receive error: %s", esp_err_to_name(err));
            }
            continue;
        }

        uint8_t first_byte = data.data[0];

        if (first_byte == NET_PKT_TYPE_HEARTBEAT) {
            if (esp_mesh_is_root() && data.size >= sizeof(mesh_heartbeat_t)) {
                mesh_heartbeat_t *hb = (mesh_heartbeat_t *)data.data;
                bool same_child = (memcmp(&from, &nearest_child_addr, 6) == 0);
                bool uninit = (nearest_child_rssi == -100);
                bool better = (hb->rssi > nearest_child_rssi);

                if (uninit || better) {
                    memcpy(&nearest_child_addr, &from, sizeof(mesh_addr_t));
                    nearest_child_rssi = hb->rssi;
                } else if (same_child) {
                    nearest_child_rssi = hb->rssi;
                }
                ESP_LOGI(TAG, "Child heartbeat: %s RSSI=%d dBm", hb->src_id, nearest_child_rssi);

                if (heartbeat_rx_callback) {
                    heartbeat_rx_callback(from.addr, hb);
                }
            }
        } else if (first_byte == NET_PKT_TYPE_PING) {
            if (data.size >= sizeof(mesh_ping_t)) {
                mesh_ping_t *ping = (mesh_ping_t *)data.data;
                mesh_ping_handle_ping(&from, ping);
            }
        } else if (first_byte == NET_PKT_TYPE_PONG) {
            if (data.size >= sizeof(mesh_ping_t)) {
                mesh_ping_t *pong = (mesh_ping_t *)data.data;
                mesh_ping_handle_pong(pong);
            }
        } else if (first_byte == NET_PKT_TYPE_STREAM_ANNOUNCE) {
            ESP_LOGD(TAG, "Stream announcement received");
        } else if (first_byte == NET_PKT_TYPE_CONTROL) {
            uplink_ctrl_message_t uplink_msg;
            if (uplink_ctrl_decode((const uplink_ctrl_packet_t *)data.data, data.size, &uplink_msg)) {
                mesh_uplink_handle_control(&uplink_msg);
            }
        } else if (first_byte == NET_FRAME_MAGIC) {
            if (data.size < NET_FRAME_HEADER_SIZE_V1) {
                continue;
            }

            net_frame_header_t *hdr = (net_frame_header_t *)data.data;
            if (hdr->version != NET_FRAME_VERSION) {
                continue;
            }

            uint16_t seq = ntohs(hdr->seq);

            if (hdr->type == NET_PKT_TYPE_AUDIO_RAW || hdr->type == NET_PKT_TYPE_AUDIO_OPUS) {
                static uint32_t audio_frames_rx = 0;
                audio_frames_rx++;
                if ((audio_frames_rx % 500) == 1) {
                    ESP_LOGI(TAG, "Audio frame RX #%lu: seq=%u size=%d",
                             audio_frames_rx, seq, data.size);
                }

                if (mesh_dedupe_is_duplicate(hdr->stream_id, seq)) {
                    continue;
                }
                mesh_dedupe_mark_seen(hdr->stream_id, seq);

                if (hdr->ttl == 0) {
                    continue;
                }

                hdr->ttl--;

                if (audio_rx_callback) {
                    uint16_t total_payload_len = ntohs(hdr->payload_len);
                    size_t hdr_size = 0;
                    if (!network_frame_resolve_header_size(data.size,
                                                           total_payload_len,
                                                           NET_FRAME_HEADER_SIZE,
                                                           NET_FRAME_HEADER_SIZE_V1,
                                                           &hdr_size)) {
                        continue;
                    }

                    uint8_t *payload = data.data + hdr_size;
                    uint32_t timestamp = ntohl(hdr->timestamp);
                    uint8_t frame_count = network_frame_extract_frame_count(data.data,
                                                                            data.size,
                                                                            NET_FRAME_HEADER_SIZE,
                                                                            hdr_size,
                                                                            hdr->frame_count,
                                                                            13);
                    const char *src_id = (hdr_size == NET_FRAME_HEADER_SIZE) ? hdr->src_id : "";

                    if (frame_count <= 1) {
                        audio_rx_callback(payload, total_payload_len, seq, timestamp, src_id);
                    } else {
                        audio_batch_callback_ctx_t cb_ctx = {.timestamp = timestamp, .src_id = src_id};
                        network_frame_unpack_batch(payload,
                                                   total_payload_len,
                                                   frame_count,
                                                   seq,
                                                   on_audio_batch_frame,
                                                   &cb_ctx);
                    }
                } else {
                    static uint32_t cb_missing = 0;
                    if ((++cb_missing % 200) == 1) {
                        ESP_LOGW(TAG, "Audio frame received but audio_rx_callback is NULL");
                    }
                }
            }
        }
    }
}
