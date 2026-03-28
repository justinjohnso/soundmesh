#include "control/usb_portal.h"
#include "control/portal_control_plane.h"
#include "control/portal_state.h"
#include "control/portal_ota.h"
#include "control/json_extract.h"
#include "config/build.h"
#include <esp_log.h>
#include <esp_http_server.h>
#include <esp_spiffs.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "portal_http";

static httpd_handle_t server = NULL;
static int ws_fd = -1;
static TaskHandle_t ws_push_task_handle = NULL;
static bool s_spiffs_ready = false;
static bool s_spiffs_owned = false;

static char ws_json_buf[4096];

// ---- SPIFFS Initialization ----

static esp_err_t init_spiffs(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "spiffs",
        .max_files = 5,
        .format_if_mount_failed = false
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    size_t total = 0, used = 0;
    esp_spiffs_info("spiffs", &total, &used);
    ESP_LOGI(TAG, "SPIFFS: %d/%d bytes used", used, total);
    return ESP_OK;
}

// ---- Static File Serving ----

static const char *get_mime_type(const char *path) {
    if (strstr(path, ".html")) return "text/html";
    if (strstr(path, ".js"))   return "application/javascript";
    if (strstr(path, ".css"))  return "text/css";
    if (strstr(path, ".json")) return "application/json";
    if (strstr(path, ".png"))  return "image/png";
    if (strstr(path, ".ico"))  return "image/x-icon";
    return "application/octet-stream";
}

static esp_err_t serve_file(httpd_req_t *req, const char *filepath) {
    char gz_path[128];
    snprintf(gz_path, sizeof(gz_path), "%s.gz", filepath);
    
    struct stat st;
    bool gzipped = false;
    const char *actual_path = filepath;
    
    if (stat(gz_path, &st) == 0) {
        actual_path = gz_path;
        gzipped = true;
    } else if (stat(filepath, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    
    FILE *f = fopen(actual_path, "r");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    
    httpd_resp_set_type(req, get_mime_type(filepath));
    if (gzipped) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
    
    char chunk[512];
    size_t read_bytes;
    while ((read_bytes = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        httpd_resp_send_chunk(req, chunk, read_bytes);
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    
    return ESP_OK;
}

// ---- HTTP Handlers ----

static esp_err_t handle_root(httpd_req_t *req) {
    esp_err_t ret = serve_file(req, "/spiffs/index.html");
    if (ret != ESP_OK) {
        const char *fallback = "<!DOCTYPE html><html><body>"
            "<h1>SoundMesh Portal</h1>"
            "<p>Web UI not loaded. Upload SPIFFS image with: <code>pio run -e src -t uploadfs</code></p>"
            "</body></html>";
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, fallback, strlen(fallback));
    }
    return ESP_OK;
}

static esp_err_t handle_static(httpd_req_t *req) {
    char filepath[600];
    snprintf(filepath, sizeof(filepath), "/spiffs%s", req->uri);
    
    esp_err_t ret = serve_file(req, filepath);
    if (ret != ESP_OK) {
        httpd_resp_send_404(req);
    }
    return ESP_OK;
}

static esp_err_t handle_api_status(httpd_req_t *req) {
    int len = portal_state_serialize_json(ws_json_buf, sizeof(ws_json_buf));
    if (len <= 0 || len >= (int)sizeof(ws_json_buf)) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"status_serialize_failed\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, ws_json_buf, len);
    return ESP_OK;
}

static esp_err_t handle_api_control_metrics(httpd_req_t *req) {
    if (req->method != HTTP_GET) {
        httpd_resp_set_status(req, "405 Method Not Allowed");
        httpd_resp_send(req, "{}", 2);
        return ESP_OK;
    }
    char buf[PORTAL_CONTROL_METRICS_JSON_BUF_SIZE];
    int len = portal_control_plane_serialize_metrics_json(buf, sizeof(buf));
    if (len <= 0 || len >= (int)sizeof(buf)) {
        ESP_LOGE(
            TAG,
            "Control metrics serialization failed (len=%d, buf=%u)",
            len,
            (unsigned)sizeof(buf));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"metrics_serialize_failed\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

static esp_err_t handle_api_ota(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        char buf[512];
        int len = portal_ota_serialize_json(buf, sizeof(buf));
        if (len <= 0 || len >= (int)sizeof(buf)) {
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_send(req, "{\"error\":\"ota_status_serialize_failed\"}", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, buf, len);
        return ESP_OK;
    }

    if (req->method != HTTP_POST) {
        httpd_resp_set_status(req, "405 Method Not Allowed");
        httpd_resp_send(req, "{}", 2);
        return ESP_OK;
    }

    portal_control_plane_record_request(PORTAL_CONTROL_ENDPOINT_OTA);

    if (!portal_control_plane_request_has_valid_token(req)) {
        ESP_LOGW(TAG, "Rejecting unauthorized OTA control request");
        return portal_control_plane_send_unauthorized(req);
    }
    if (!portal_control_plane_allow_rate_limited_request(PORTAL_CONTROL_ENDPOINT_OTA, "/api/ota")) {
        return portal_control_plane_send_rate_limited(req);
    }

    char body[256] = {0};
    int rcvd = httpd_req_recv(req, body, sizeof(body) - 1);
    if (rcvd <= 0) {
        portal_control_plane_record_bad_request();
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"error\":\"empty body\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char url[192];
    if (!json_extract_string_field(body, "url", url, sizeof(url))) {
        portal_control_plane_record_bad_request();
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"error\":\"missing url\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (url[0] == '\0') {
        portal_control_plane_record_bad_request();
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"error\":\"invalid url\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    esp_err_t ret = portal_ota_start(url);
    if (ret != ESP_OK) {
        portal_control_plane_record_apply_failure(PORTAL_CONTROL_ENDPOINT_OTA);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_send(req, "{\"error\":\"ota start failed\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t handle_api_uplink(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        network_uplink_status_t st = {0};
        network_get_uplink_status(&st);
        char buf[320];
        int len = snprintf(
            buf,
            sizeof(buf),
            "{\"enabled\":%s,\"configured\":%s,\"rootApplied\":%s,"
            "\"pendingApply\":%s,\"ssid\":\"%s\",\"lastError\":\"%s\",\"updatedMs\":%lu}",
            st.enabled ? "true" : "false",
            st.configured ? "true" : "false",
            st.root_applied ? "true" : "false",
            st.pending_apply ? "true" : "false",
            st.configured ? "<configured>" : "",
            st.last_error,
            (unsigned long)st.updated_ms);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, buf, len);
        return ESP_OK;
    }

    if (req->method != HTTP_POST) {
        httpd_resp_set_status(req, "405 Method Not Allowed");
        httpd_resp_send(req, "{}", 2);
        return ESP_OK;
    }

    portal_control_plane_record_request(PORTAL_CONTROL_ENDPOINT_UPLINK);

    if (!portal_control_plane_request_has_valid_token(req)) {
        ESP_LOGW(TAG, "Rejecting unauthorized uplink control request");
        return portal_control_plane_send_unauthorized(req);
    }
    if (!portal_control_plane_allow_rate_limited_request(PORTAL_CONTROL_ENDPOINT_UPLINK, "/api/uplink")) {
        return portal_control_plane_send_rate_limited(req);
    }

    char body[320] = {0};
    int rcvd = httpd_req_recv(req, body, sizeof(body) - 1);
    if (rcvd <= 0) {
        portal_control_plane_record_bad_request();
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"error\":\"empty body\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    bool enabled = false;
    if (!json_extract_bool_field(body, "enabled", &enabled)) {
        portal_control_plane_record_bad_request();
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"error\":\"missing enabled\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char ssid[UPLINK_SSID_MAX_LEN + 1] = {0};
    char password[UPLINK_PASSWORD_MAX_LEN + 1] = {0};
    if (enabled) {
        if (!json_extract_string_field(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
            portal_control_plane_record_bad_request();
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_send(req, "{\"error\":\"missing ssid\"}", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        if (!json_extract_string_field(body, "password", password, sizeof(password))) {
            password[0] = '\0';
        }
    }

    esp_err_t ret = network_set_uplink_config(ssid, password, enabled);
    if (ret != ESP_OK) {
        portal_control_plane_record_apply_failure(PORTAL_CONTROL_ENDPOINT_UPLINK);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_send(req, "{\"error\":\"uplink apply failed\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t handle_api_mixer(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        network_mixer_status_t st = {0};
        if (network_get_mixer_status(&st) != ESP_OK) {
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_send(req, "{\"error\":\"mixer_status_failed\"}", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        char buf[256];
        int len = snprintf(
            buf,
            sizeof(buf),
            "{\"outGainPct\":%u,\"applied\":%s,\"pendingApply\":%s,\"lastError\":\"%s\",\"updatedMs\":%lu}",
            (unsigned)st.out_gain_pct,
            st.applied ? "true" : "false",
            st.pending_apply ? "true" : "false",
            st.last_error,
            (unsigned long)st.updated_ms);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, buf, len);
        return ESP_OK;
    }

    if (req->method != HTTP_POST) {
        httpd_resp_set_status(req, "405 Method Not Allowed");
        httpd_resp_send(req, "{}", 2);
        return ESP_OK;
    }

    portal_control_plane_record_request(PORTAL_CONTROL_ENDPOINT_MIXER);

    if (!portal_control_plane_request_has_valid_token(req)) {
        ESP_LOGW(TAG, "Rejecting unauthorized mixer control request");
        return portal_control_plane_send_unauthorized(req);
    }
    if (!portal_control_plane_allow_rate_limited_request(PORTAL_CONTROL_ENDPOINT_MIXER, "/api/mixer")) {
        return portal_control_plane_send_rate_limited(req);
    }

    char body[128] = {0};
    int rcvd = httpd_req_recv(req, body, sizeof(body) - 1);
    if (rcvd <= 0) {
        portal_control_plane_record_bad_request();
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"error\":\"empty body\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    uint16_t out_gain_pct = 0;
    if (!json_extract_uint16_field(body, "outGainPct", &out_gain_pct)) {
        portal_control_plane_record_bad_request();
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"error\":\"missing outGainPct\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (out_gain_pct < MIXER_OUT_GAIN_MIN_PCT || out_gain_pct > MIXER_OUT_GAIN_MAX_PCT) {
        portal_control_plane_record_bad_request();
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"error\":\"invalid outGainPct\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    esp_err_t ret = network_set_mixer_config(out_gain_pct);
    if (ret != ESP_OK) {
        portal_control_plane_record_apply_failure(PORTAL_CONTROL_ENDPOINT_MIXER);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_send(req, "{\"error\":\"mixer apply failed\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t handle_captive_redirect(httpd_req_t *req) {
    const esp_netif_ip_info_t *info = portal_get_ip_info();
    char location[48];
    snprintf(location, sizeof(location), "http://" IPSTR "/", IP2STR(&info->ip));
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ---- WebSocket ----

static esp_err_t handle_ws(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        if (!portal_control_plane_request_has_valid_token(req)) {
            ESP_LOGW(TAG, "Rejecting unauthorized WS control connection");
            return portal_control_plane_send_unauthorized(req);
        }
        ws_fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "WebSocket connected, fd=%d", ws_fd);
        return ESP_OK;
    }
    
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(ws_pkt));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "WS recv error: %s", esp_err_to_name(ret));
        ws_fd = -1;
    }
    return ESP_OK;
}

static void ws_push_task(void *arg) {
    httpd_handle_t srv = (httpd_handle_t)arg;
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        
        if (ws_fd < 0 || srv == NULL) continue;
        
        int len = portal_state_serialize_json(ws_json_buf, sizeof(ws_json_buf));
        if (len <= 0) continue;
        
        httpd_ws_frame_t ws_pkt = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)ws_json_buf,
            .len = len,
            .final = true
        };
        
        esp_err_t ret = httpd_ws_send_frame_async(srv, ws_fd, &ws_pkt);
        if (ret != ESP_OK) {
            ESP_LOGD(TAG, "WS send failed: %s, closing", esp_err_to_name(ret));
            ws_fd = -1;
        }
    }
}

// ---- Server Start ----

esp_err_t portal_http_start(void) {
    init_spiffs();
    portal_control_plane_reset();
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_open_sockets = 4;
    config.max_uri_handlers = 16;
    config.lru_purge_enable = true;
    // Let wildcard handler catch unrecognized captive-check URIs from different OSes.
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 6144;
    
    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    httpd_uri_t uri_root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = handle_root
    };
    httpd_register_uri_handler(server, &uri_root);
    
    httpd_uri_t uri_api = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = handle_api_status
    };
    httpd_register_uri_handler(server, &uri_api);

    httpd_uri_t uri_control_metrics = {
        .uri = "/api/control/metrics",
        .method = HTTP_GET,
        .handler = handle_api_control_metrics
    };
    httpd_register_uri_handler(server, &uri_control_metrics);

    httpd_uri_t uri_ota_get = {
        .uri = "/api/ota",
        .method = HTTP_GET,
        .handler = handle_api_ota
    };
    httpd_uri_t uri_ota_post = {
        .uri = "/api/ota",
        .method = HTTP_POST,
        .handler = handle_api_ota
    };
    httpd_uri_t uri_uplink_get = {
        .uri = "/api/uplink",
        .method = HTTP_GET,
        .handler = handle_api_uplink
    };
    httpd_uri_t uri_uplink_post = {
        .uri = "/api/uplink",
        .method = HTTP_POST,
        .handler = handle_api_uplink
    };
    httpd_uri_t uri_mixer_get = {
        .uri = "/api/mixer",
        .method = HTTP_GET,
        .handler = handle_api_mixer
    };
    httpd_uri_t uri_mixer_post = {
        .uri = "/api/mixer",
        .method = HTTP_POST,
        .handler = handle_api_mixer
    };
    httpd_register_uri_handler(server, &uri_ota_get);
    httpd_register_uri_handler(server, &uri_ota_post);
    httpd_register_uri_handler(server, &uri_uplink_get);
    httpd_register_uri_handler(server, &uri_uplink_post);
    httpd_register_uri_handler(server, &uri_mixer_get);
    httpd_register_uri_handler(server, &uri_mixer_post);
    
    httpd_uri_t uri_ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = handle_ws,
        .is_websocket = true
    };
    httpd_register_uri_handler(server, &uri_ws);
    
    httpd_uri_t uri_js = { .uri = "/app.js", .method = HTTP_GET, .handler = handle_static };
    httpd_uri_t uri_css = { .uri = "/app.css", .method = HTTP_GET, .handler = handle_static };
    httpd_register_uri_handler(server, &uri_js);
    httpd_register_uri_handler(server, &uri_css);
    
    const char *probe_uris[] = {
        "/generate_204",
        "/gen_204",
        "/mobile/status.php",
        "/connectivity-check.html",
        "/redirect",
        "/redirect.html",
        "/hotspot-detect.html",
        "/hotspotdetect.html",
        "/success.html",
        "/library/test/success.html",
        "/connecttest.txt",
        "/ncsi.txt",
        "/fwlink",
        "/msftconnecttest/redirect",
        "/success.txt",
        "/canonical.html"
    };
    for (int i = 0; i < sizeof(probe_uris) / sizeof(probe_uris[0]); i++) {
        httpd_uri_t uri_probe = {
            .uri = probe_uris[i],
            .method = HTTP_GET,
            .handler = handle_captive_redirect
        };
        httpd_register_uri_handler(server, &uri_probe);
    }

    httpd_uri_t uri_catch_all = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = handle_captive_redirect
    };
    httpd_register_uri_handler(server, &uri_catch_all);
    
    BaseType_t ws_task_created = xTaskCreatePinnedToCore(
        ws_push_task, "ws_push", PORTAL_WS_PUSH_STACK_BYTES, server, 3, &ws_push_task_handle, 0);
    if (ws_task_created != pdPASS || ws_push_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create ws_push task");
        httpd_stop(server);
        server = NULL;
        ws_fd = -1;
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "HTTP server started on port 80");
    return ESP_OK;
}
