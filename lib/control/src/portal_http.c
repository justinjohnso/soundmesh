#include "control/usb_portal.h"
#include "control/portal_state.h"
#include "config/build.h"
#include <esp_log.h>
#include <esp_http_server.h>
#include <esp_spiffs.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "portal_http";

static httpd_handle_t server = NULL;
static int ws_fd = -1;
static TaskHandle_t ws_push_task_handle = NULL;

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
            "<p>Web UI not loaded. Upload SPIFFS image with: <code>pio run -e tx -t uploadfs</code></p>"
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
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, ws_json_buf, len);
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
    
    xTaskCreatePinnedToCore(ws_push_task, "ws_push", 4096, server, 3, &ws_push_task_handle, 0);
    
    ESP_LOGI(TAG, "HTTP server started on port 80");
    return ESP_OK;
}
