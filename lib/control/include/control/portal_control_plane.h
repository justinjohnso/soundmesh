#pragma once

#include <esp_err.h>
#include <esp_http_server.h>
#include <stddef.h>
#include <stdbool.h>

typedef enum {
    PORTAL_CONTROL_ENDPOINT_OTA = 0,
    PORTAL_CONTROL_ENDPOINT_UPLINK,
    PORTAL_CONTROL_ENDPOINT_MIXER,
    PORTAL_CONTROL_ENDPOINT_MESH_POSITIONS,
} portal_control_endpoint_t;

void portal_control_plane_reset(void);
void portal_control_plane_record_request(portal_control_endpoint_t endpoint);
void portal_control_plane_record_apply_failure(portal_control_endpoint_t endpoint);
void portal_control_plane_record_bad_request(void);

bool portal_control_plane_request_has_valid_token(httpd_req_t *req);
bool portal_control_plane_allow_rate_limited_request(portal_control_endpoint_t endpoint, const char *path_label);

esp_err_t portal_control_plane_send_unauthorized(httpd_req_t *req);
esp_err_t portal_control_plane_send_rate_limited(httpd_req_t *req);

int portal_control_plane_serialize_metrics_json(char *buf, size_t buf_len);
