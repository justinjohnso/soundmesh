#include "mesh/mesh_rx.h"
#include "mesh/mesh_state.h"
#include "mesh/mesh_dedupe.h"
#include "mesh/mesh_ping.h"
#include "mesh/mesh_uplink.h"
#include "mesh/mesh_mixer.h"
#include "network/uplink_control.h"
#include "network/mixer_control.h"
#include "network/frame_codec.h"
#include "network/mesh_net.h"
#include "config/build.h"
#include <esp_timer.h>
#include <esp_log.h>
#include <esp_mesh.h>
#include <string.h>

static const char *TAG = "network_mesh";

static void mesh_rx_log_observability_snapshot(void)
{
    ESP_LOGI(TAG,
             "RX OBS: audio=%lu fwd=%lu dup=%lu ttl0=%lu inv={hdr:%lu ver:%lu pay:%lu} "
             "batch={pkts:%lu frames:%lu} cb_miss=%lu recv={err:%lu empty:%lu} "
             "burst_loss=%lu burst_max=%lu jitter_us=%lu ctrl={hb:%lu ctl:%lu ping:%lu pong:%lu ann:%lu} "
             "churn={pc:%lu pd:%lu np:%lu sc:%lu rj:%lu/%lu/%lu}",
             (unsigned long)g_transport_stats.rx_audio_packets,
             (unsigned long)g_transport_stats.rx_audio_forwarded,
             (unsigned long)g_transport_stats.rx_audio_duplicates,
             (unsigned long)g_transport_stats.rx_audio_ttl_expired,
             (unsigned long)g_transport_stats.rx_audio_invalid_header,
             (unsigned long)g_transport_stats.rx_audio_invalid_version,
             (unsigned long)g_transport_stats.rx_audio_invalid_payload,
             (unsigned long)g_transport_stats.rx_audio_batches,
             (unsigned long)g_transport_stats.rx_audio_batch_frames,
             (unsigned long)g_transport_stats.rx_audio_callback_missing,
             (unsigned long)g_transport_stats.mesh_recv_errors,
             (unsigned long)g_transport_stats.mesh_recv_empty_packets,
             (unsigned long)g_transport_stats.rx_audio_burst_loss_events,
             (unsigned long)g_transport_stats.rx_audio_burst_loss_max,
             (unsigned long)g_transport_stats.rx_audio_interarrival_jitter_us,
             (unsigned long)g_transport_stats.rx_heartbeat_packets,
             (unsigned long)g_transport_stats.rx_control_packets,
             (unsigned long)g_transport_stats.rx_ping_packets,
             (unsigned long)g_transport_stats.rx_pong_packets,
             (unsigned long)g_transport_stats.rx_stream_announce_packets,
             (unsigned long)g_transport_stats.parent_connect_events,
             (unsigned long)g_transport_stats.parent_disconnect_events,
             (unsigned long)g_transport_stats.no_parent_events,
             (unsigned long)g_transport_stats.scan_done_events,
             (unsigned long)g_transport_stats.rejoin_trigger_events,
             (unsigned long)g_transport_stats.rejoin_blocked_events,
             (unsigned long)g_transport_stats.rejoin_circuit_breaker_events);
}

static void mesh_rx_update_audio_loss_and_jitter(uint16_t seq,
                                                 uint8_t frame_count,
                                                 uint32_t sender_timestamp_ms)
{
    static bool initialized = false;
    static uint16_t expected_next_seq = 0;
    static uint64_t last_arrival_us = 0;
    static uint32_t last_sender_timestamp_ms = 0;

    uint8_t effective_frame_count = frame_count > 0 ? frame_count : 1;
    uint64_t now_us = (uint64_t)esp_timer_get_time();

    if (initialized) {
        int16_t seq_delta = (int16_t)(seq - expected_next_seq);
        if (seq_delta > 0) {
            uint32_t missing_frames = (uint32_t)seq_delta;
            if (missing_frames >= RX_BURST_LOSS_THRESHOLD) {
                g_transport_stats.rx_audio_burst_loss_events++;
                if (missing_frames > g_transport_stats.rx_audio_burst_loss_max) {
                    g_transport_stats.rx_audio_burst_loss_max = missing_frames;
                }
            }
        }

        uint64_t arrival_delta_us = now_us - last_arrival_us;
        uint32_t sender_delta_ms = sender_timestamp_ms - last_sender_timestamp_ms;
        uint64_t sender_delta_us = (uint64_t)sender_delta_ms * 1000ULL;
        int64_t transit_delta_us = (int64_t)arrival_delta_us - (int64_t)sender_delta_us;
        uint64_t abs_transit_delta_us =
            (transit_delta_us >= 0) ? (uint64_t)transit_delta_us : (uint64_t)(-transit_delta_us);

        int64_t jitter_us = (int64_t)g_transport_stats.rx_audio_interarrival_jitter_us;
        jitter_us += ((int64_t)abs_transit_delta_us - jitter_us) / 16;
        if (jitter_us < 0) {
            jitter_us = 0;
        }
        g_transport_stats.rx_audio_interarrival_jitter_us = (uint32_t)jitter_us;
    } else {
        initialized = true;
    }

    expected_next_seq = (uint16_t)(seq + effective_frame_count);
    last_arrival_us = now_us;
    last_sender_timestamp_ms = sender_timestamp_ms;
}

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
    g_transport_stats.rx_audio_forwarded++;
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
            g_transport_stats.mesh_recv_errors++;
            if (err == ESP_ERR_MESH_NOT_START) {
                vTaskDelay(pdMS_TO_TICKS(100));
            } else {
                ESP_LOGW(TAG, "Mesh receive error: %s", esp_err_to_name(err));
            }
            continue;
        }

        if (!data.data || data.size == 0) {
            g_transport_stats.mesh_recv_empty_packets++;
            ESP_LOGW(TAG, "Ignoring empty mesh packet");
            continue;
        }

        uint8_t first_byte = data.data[0];

        if (first_byte == NET_PKT_TYPE_HEARTBEAT) {
            g_transport_stats.rx_heartbeat_packets++;
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
            g_transport_stats.rx_ping_packets++;
            if (data.size >= sizeof(mesh_ping_t)) {
                mesh_ping_t *ping = (mesh_ping_t *)data.data;
                mesh_ping_handle_ping(&from, ping);
            }
        } else if (first_byte == NET_PKT_TYPE_PONG) {
            g_transport_stats.rx_pong_packets++;
            if (data.size >= sizeof(mesh_ping_t)) {
                mesh_ping_t *pong = (mesh_ping_t *)data.data;
                mesh_ping_handle_pong(pong);
            }
        } else if (first_byte == NET_PKT_TYPE_STREAM_ANNOUNCE) {
            g_transport_stats.rx_stream_announce_packets++;
            ESP_LOGD(TAG, "Stream announcement received");
        } else if (first_byte == NET_PKT_TYPE_CONTROL) {
            g_transport_stats.rx_control_packets++;
            if (data.size == sizeof(uplink_ctrl_packet_t)) {
                uplink_ctrl_message_t uplink_msg;
                if (uplink_ctrl_decode((const uplink_ctrl_packet_t *)data.data, data.size, &uplink_msg)) {
                    mesh_uplink_handle_control(&uplink_msg);
                } else {
                    ESP_LOGW(TAG, "Ignoring invalid uplink control packet");
                }
            } else if (data.size == sizeof(mixer_ctrl_packet_t)) {
                mixer_ctrl_message_t mixer_msg;
                if (mixer_ctrl_decode((const mixer_ctrl_packet_t *)data.data, data.size, &mixer_msg)) {
                    mesh_mixer_handle_control(&mixer_msg);
                } else {
                    ESP_LOGW(TAG, "Ignoring invalid mixer control packet");
                }
            } else {
                ESP_LOGW(TAG, "Ignoring unknown control packet size=%d", data.size);
            }
        } else if (first_byte == NET_FRAME_MAGIC) {
            if (data.size < NET_FRAME_HEADER_SIZE_V1) {
                g_transport_stats.rx_audio_invalid_header++;
                continue;
            }

            net_frame_header_t *hdr = (net_frame_header_t *)data.data;
            if (hdr->version != NET_FRAME_VERSION) {
                g_transport_stats.rx_audio_invalid_version++;
                continue;
            }

            uint16_t seq = ntohs(hdr->seq);

            if (hdr->type == NET_PKT_TYPE_AUDIO_RAW || hdr->type == NET_PKT_TYPE_AUDIO_OPUS) {
                g_transport_stats.rx_audio_packets++;
                static uint64_t last_obs_log_us = 0;
                uint64_t now_us = (uint64_t)esp_timer_get_time();
                uint64_t telemetry_interval_us = (uint64_t)CONTROL_TELEMETRY_RATE_MS * 1000ULL;
                if (last_obs_log_us == 0 || (now_us - last_obs_log_us) >= telemetry_interval_us) {
                    ESP_LOGI(TAG, "Audio frame RX #%lu: seq=%u size=%d",
                             (unsigned long)g_transport_stats.rx_audio_packets, seq, data.size);
                    mesh_rx_log_observability_snapshot();
                    last_obs_log_us = now_us;
                }

                if (mesh_dedupe_is_duplicate(hdr->stream_id, seq)) {
                    g_transport_stats.rx_audio_duplicates++;
                    continue;
                }
                mesh_dedupe_mark_seen(hdr->stream_id, seq);

                if (hdr->ttl == 0) {
                    g_transport_stats.rx_audio_ttl_expired++;
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
                        g_transport_stats.rx_audio_invalid_payload++;
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
                    uint8_t effective_frame_count = frame_count > 0 ? frame_count : 1;
                    const char *src_id = (hdr_size == NET_FRAME_HEADER_SIZE) ? hdr->src_id : "";
                    mesh_rx_update_audio_loss_and_jitter(seq, effective_frame_count, timestamp);

                    if (effective_frame_count <= 1) {
                        g_transport_stats.rx_audio_forwarded++;
                        audio_rx_callback(payload, total_payload_len, seq, timestamp, src_id);
                    } else {
                        audio_batch_callback_ctx_t cb_ctx = {.timestamp = timestamp, .src_id = src_id};
                        g_transport_stats.rx_audio_batches++;
                        g_transport_stats.rx_audio_batch_frames += effective_frame_count;
                        network_frame_unpack_batch(payload,
                                                   total_payload_len,
                                                   effective_frame_count,
                                                   seq,
                                                   on_audio_batch_frame,
                                                   &cb_ctx);
                    }
                } else {
                    g_transport_stats.rx_audio_callback_missing++;
                    if ((g_transport_stats.rx_audio_callback_missing % 200) == 1) {
                        ESP_LOGW(TAG, "Audio frame received but audio_rx_callback is NULL");
                    }
                }
            }
        }
    }
}
