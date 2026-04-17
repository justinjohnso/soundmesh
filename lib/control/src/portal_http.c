#include "control/usb_portal.h"
#include "control/portal_control_plane.h"
#include "control/portal_state.h"
#include "control/portal_ota.h"
#include "control/json_extract.h"
#include "network/uplink_control.h"
#include "network/mixer_control.h"
#include "config/build.h"
#include <esp_log.h>
#include <esp_http_server.h>
#include <esp_spiffs.h>
#include <esp_vfs.h>
#include <esp_timer.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>

#if BUILD_HAS_PORTAL

static const char *TAG = "portal_http";
static httpd_handle_t server = NULL;
static int ws_fd = -1;
static TaskHandle_t ws_push_task_handle = NULL;
static char ws_json_buf[1024];

static void init_spiffs(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs", .partition_label = NULL,
        .max_files = 5, .format_if_mount_failed = false
    };
    esp_vfs_spiffs_register(&conf);
}

static esp_err_t handle_static(httpd_req_t *req) {
    char path[128];
    if (strcmp(req->uri, "/") == 0) strcpy(path, "/spiffs/index.html");
    else snprintf(path, sizeof(path), "/spiffs%s", req->uri);
    
    struct stat st;
    if (stat(path, &st) != 0) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    if (strstr(path, ".js")) httpd_resp_set_type(req, "application/javascript");
    else if (strstr(path, ".css")) httpd_resp_set_type(req, "text/css");
    else if (strstr(path, ".html")) httpd_resp_set_type(req, "text/html");
    
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");

    char buf[1024];
    size_t read;
    while ((read = fread(buf, 1, sizeof(buf), f)) > 0) {
        httpd_resp_send_chunk(req, buf, read);
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_api_status(httpd_req_t *req) {
    char buf[1024];
    int len = portal_state_serialize_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

static esp_err_t handle_api_ota(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        char buf[256];
        int len = portal_ota_serialize_json(buf, sizeof(buf));
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, buf, len);
    }
    
    portal_control_plane_record_request(PORTAL_CONTROL_ENDPOINT_OTA);
    if (!portal_control_plane_request_has_valid_token(req)) return portal_control_plane_send_unauthorized(req);
    
    char body[256];
    int r = httpd_req_recv(req, body, sizeof(body)-1);
    if (r <= 0) return ESP_FAIL;
    body[r] = 0;

    char url[128];
    if (json_extract_string_field(body, "\"url\"", url, sizeof(url))) {
        if (portal_ota_start(url) == ESP_OK) {
            httpd_resp_send(req, "{\"status\":\"started\"}", -1);
        } else {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_send(req, "{\"error\":\"ota busy\"}", -1);
        }
    }
    return ESP_OK;
}

// These are missing in headers but present in .c files - we will add them locally for now
extern int uplink_ctrl_serialize_json(char *buf, size_t buf_len);
extern esp_err_t uplink_ctrl_decode_json(const char *json, uplink_ctrl_message_t *msg);
extern esp_err_t uplink_ctrl_apply(const uplink_ctrl_message_t *msg);

static esp_err_t handle_api_uplink(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        char buf[256];
        int len = uplink_ctrl_serialize_json(buf, sizeof(buf));
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, buf, len);
    }
    
    portal_control_plane_record_request(PORTAL_CONTROL_ENDPOINT_UPLINK);
    if (!portal_control_plane_request_has_valid_token(req)) return portal_control_plane_send_unauthorized(req);

    char body[256];
    int r = httpd_req_recv(req, body, sizeof(body)-1);
    if (r <= 0) return ESP_FAIL;
    body[r] = 0;

    uplink_ctrl_message_t msg;
    if (uplink_ctrl_decode_json(body, &msg) == ESP_OK) {
        if (uplink_ctrl_apply(&msg) == ESP_OK) {
            httpd_resp_send(req, "{\"status\":\"applied\"}", -1);
        } else {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_send(req, "{\"error\":\"apply failed\"}", -1);
        }
    }
    return ESP_OK;
}

extern int mixer_ctrl_serialize_json(char *buf, size_t buf_len);
extern esp_err_t mixer_ctrl_decode_json(const char *json, mixer_ctrl_message_t *msg);
extern esp_err_t mixer_ctrl_apply(const mixer_ctrl_message_t *msg);

static esp_err_t handle_api_mixer(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        char buf[512];
        int len = mixer_ctrl_serialize_json(buf, sizeof(buf));
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, buf, len);
    }
    
    portal_control_plane_record_request(PORTAL_CONTROL_ENDPOINT_MIXER);
    if (!portal_control_plane_request_has_valid_token(req)) return portal_control_plane_send_unauthorized(req);

    char body[512];
    int r = httpd_req_recv(req, body, sizeof(body)-1);
    if (r <= 0) return ESP_FAIL;
    body[r] = 0;

    mixer_ctrl_message_t msg;
    if (mixer_ctrl_decode_json(body, &msg) == ESP_OK) {
        if (mixer_ctrl_apply(&msg) == ESP_OK) {
            httpd_resp_send(req, "{\"status\":\"applied\"}", -1);
        } else {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_send(req, "{\"error\":\"apply failed\"}", -1);
        }
    }
    return ESP_OK;
}

static esp_err_t handle_api_mesh_positions(httpd_req_t *req) {
    portal_control_plane_record_request(PORTAL_CONTROL_ENDPOINT_MESH_POSITIONS);
    if (!portal_control_plane_request_has_valid_token(req)) return portal_control_plane_send_unauthorized(req);
    
    char body[512];
    int r = httpd_req_recv(req, body, sizeof(body)-1);
    if (r <= 0) return ESP_FAIL;
    body[r] = 0;
    
    httpd_resp_send(req, "{\"status\":\"received\"}", -1);
    return ESP_OK;
}

static esp_err_t handle_api_control_metrics(httpd_req_t *req) {
    char buf[512];
    int len = portal_control_plane_serialize_metrics_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

static esp_err_t handle_captive_redirect(httpd_req_t *req) {
    const esp_netif_ip_info_t *info = portal_get_ip_info();
    if (!info) return ESP_FAIL;
    char loc[64];
    snprintf(loc, sizeof(loc), "http://" IPSTR "/", IP2STR(&info->ip));
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", loc);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

#if CONFIG_HTTPD_WS_SUPPORT
static esp_err_t handle_ws(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        ws_fd = httpd_req_to_sockfd(req);
        return ESP_OK;
    }
    httpd_ws_frame_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    return httpd_ws_recv_frame(req, &pkt, 0);
}

static void ws_push_task(void *arg) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (ws_fd < 0) continue;
        int len = portal_state_serialize_json(ws_json_buf, sizeof(ws_json_buf));
        httpd_ws_frame_t pkt = { .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t*)ws_json_buf, .len = len, .final = true };
        if (httpd_ws_send_frame_async(server, ws_fd, &pkt) != ESP_OK) ws_fd = -1;
    }
}
#endif

esp_err_t portal_http_start(void) {
    init_spiffs();
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 24;
    config.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&server, &config) != ESP_OK) return ESP_FAIL;

    httpd_uri_t uris[] = {
        {"/", HTTP_GET, handle_static, NULL},
        {"/api/status", HTTP_GET, handle_api_status, NULL},
        {"/api/ota", HTTP_GET, handle_api_ota, NULL},
        {"/api/ota", HTTP_POST, handle_api_ota, NULL},
        {"/api/uplink", HTTP_GET, handle_api_uplink, NULL},
        {"/api/uplink", HTTP_POST, handle_api_uplink, NULL},
        {"/api/mixer", HTTP_GET, handle_api_mixer, NULL},
        {"/api/mixer", HTTP_POST, handle_api_mixer, NULL},
        {"/api/mesh/positions", HTTP_POST, handle_api_mesh_positions, NULL},
        {"/api/control/metrics", HTTP_GET, handle_api_control_metrics, NULL},
#if CONFIG_HTTPD_WS_SUPPORT
        {"/ws", HTTP_GET, handle_ws, NULL},
#endif
        {"*", HTTP_GET, handle_captive_redirect, NULL}
    };

    for (int i = 0; i < sizeof(uris)/sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
    
#if CONFIG_HTTPD_WS_SUPPORT
    xTaskCreate(ws_push_task, "ws_push", 4096, NULL, 3, &ws_push_task_handle);
#endif

    return ESP_OK;
}

esp_err_t portal_http_stop(void) {
    if (server) {
        httpd_stop(server);
        server = NULL;
    }
#if CONFIG_HTTPD_WS_SUPPORT
    if (ws_push_task_handle) {
        vTaskDelete(ws_push_task_handle);
        ws_push_task_handle = NULL;
    }
#endif
    return ESP_OK;
}

#else /* !BUILD_HAS_PORTAL */

esp_err_t portal_http_start(void) { return ESP_OK; }
esp_err_t portal_http_stop(void) { return ESP_OK; }

#endif
