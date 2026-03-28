#include "control/portal_control_plane.h"

#include "config/build.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "portal_control";

typedef struct {
    uint32_t window_start_ms;
    uint32_t request_count;
    uint32_t rejected_count;
} portal_rate_limiter_t;

static portal_rate_limiter_t s_ota_rate_limit = {0};
static portal_rate_limiter_t s_uplink_rate_limit = {0};
static portal_rate_limiter_t s_mixer_rate_limit = {0};
static uint32_t s_auth_reject_count = 0;
static uint32_t s_ota_requests_total = 0;
static uint32_t s_uplink_requests_total = 0;
static uint32_t s_mixer_requests_total = 0;
static uint32_t s_ota_apply_fail_count = 0;
static uint32_t s_uplink_apply_fail_count = 0;
static uint32_t s_mixer_apply_fail_count = 0;
static uint32_t s_bad_request_count = 0;
static char s_auth_token_buf[96];

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static portal_rate_limiter_t *rate_limiter_for_endpoint(portal_control_endpoint_t endpoint)
{
    switch (endpoint) {
        case PORTAL_CONTROL_ENDPOINT_OTA:
            return &s_ota_rate_limit;
        case PORTAL_CONTROL_ENDPOINT_UPLINK:
            return &s_uplink_rate_limit;
        case PORTAL_CONTROL_ENDPOINT_MIXER:
            return &s_mixer_rate_limit;
        default:
            return NULL;
    }
}

static const char *portal_auth_token(void)
{
#if PORTAL_REQUIRE_CONTROL_AUTH
    const char *env_token = getenv("SOUNDMESH_CONTROL_TOKEN");
    if (env_token && env_token[0] != '\0') {
        strlcpy(s_auth_token_buf, env_token, sizeof(s_auth_token_buf));
    } else {
        strlcpy(s_auth_token_buf, PORTAL_CONTROL_AUTH_TOKEN, sizeof(s_auth_token_buf));
    }
    return s_auth_token_buf;
#else
    return NULL;
#endif
}

void portal_control_plane_reset(void)
{
    memset(&s_ota_rate_limit, 0, sizeof(s_ota_rate_limit));
    memset(&s_uplink_rate_limit, 0, sizeof(s_uplink_rate_limit));
    memset(&s_mixer_rate_limit, 0, sizeof(s_mixer_rate_limit));
    s_auth_reject_count = 0;
    s_ota_requests_total = 0;
    s_uplink_requests_total = 0;
    s_mixer_requests_total = 0;
    s_ota_apply_fail_count = 0;
    s_uplink_apply_fail_count = 0;
    s_mixer_apply_fail_count = 0;
    s_bad_request_count = 0;
}

void portal_control_plane_record_request(portal_control_endpoint_t endpoint)
{
    if (endpoint == PORTAL_CONTROL_ENDPOINT_OTA) {
        s_ota_requests_total++;
    } else if (endpoint == PORTAL_CONTROL_ENDPOINT_UPLINK) {
        s_uplink_requests_total++;
    } else if (endpoint == PORTAL_CONTROL_ENDPOINT_MIXER) {
        s_mixer_requests_total++;
    }
}

void portal_control_plane_record_apply_failure(portal_control_endpoint_t endpoint)
{
    if (endpoint == PORTAL_CONTROL_ENDPOINT_OTA) {
        s_ota_apply_fail_count++;
    } else if (endpoint == PORTAL_CONTROL_ENDPOINT_UPLINK) {
        s_uplink_apply_fail_count++;
    } else if (endpoint == PORTAL_CONTROL_ENDPOINT_MIXER) {
        s_mixer_apply_fail_count++;
    }
}

void portal_control_plane_record_bad_request(void)
{
    s_bad_request_count++;
}

bool portal_control_plane_request_has_valid_token(httpd_req_t *req)
{
#if PORTAL_REQUIRE_CONTROL_AUTH
    const char *token = portal_auth_token();
    if (!token || token[0] == '\0') {
        ESP_LOGW(TAG, "Control auth enabled but token is empty");
        return false;
    }

    char header[160];
    size_t auth_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (auth_len > 0 && auth_len < sizeof(header) &&
        httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) == ESP_OK) {
        const char *prefix = "Bearer ";
        if (strncmp(header, prefix, strlen(prefix)) == 0) {
            const char *provided = header + strlen(prefix);
            if (strcmp(provided, token) == 0) {
                return true;
            }
        }
    }

    size_t token_len = httpd_req_get_hdr_value_len(req, "X-SoundMesh-Token");
    if (token_len > 0 && token_len < sizeof(header) &&
        httpd_req_get_hdr_value_str(req, "X-SoundMesh-Token", header, sizeof(header)) == ESP_OK &&
        strcmp(header, token) == 0) {
        return true;
    }

    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len > 0) {
        char query[160];
        char query_token[96];
        if ((query_len + 1) < sizeof(query) &&
            httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
            httpd_query_key_value(query, "token", query_token, sizeof(query_token)) == ESP_OK &&
            strcmp(query_token, token) == 0) {
            return true;
        }
    }
    return false;
#else
    (void)req;
    return true;
#endif
}

bool portal_control_plane_allow_rate_limited_request(portal_control_endpoint_t endpoint, const char *path_label)
{
    portal_rate_limiter_t *limiter = rate_limiter_for_endpoint(endpoint);
    if (!limiter || !path_label) {
        return false;
    }

    const uint32_t now = now_ms();
    if (limiter->window_start_ms == 0 ||
        (now - limiter->window_start_ms) >= PORTAL_CONTROL_RATE_LIMIT_WINDOW_MS) {
        limiter->window_start_ms = now;
        limiter->request_count = 0;
    }

    limiter->request_count++;
    if (limiter->request_count > PORTAL_CONTROL_RATE_LIMIT_MAX_REQUESTS) {
        limiter->rejected_count++;
        ESP_LOGW(TAG, "Rate limit reject on %s (window=%ums max=%u rejects=%lu)",
                 path_label,
                 (unsigned)PORTAL_CONTROL_RATE_LIMIT_WINDOW_MS,
                 (unsigned)PORTAL_CONTROL_RATE_LIMIT_MAX_REQUESTS,
                 (unsigned long)limiter->rejected_count);
        return false;
    }
    return true;
}

esp_err_t portal_control_plane_send_unauthorized(httpd_req_t *req)
{
    s_auth_reject_count++;
    ESP_LOGW(TAG, "Unauthorized control request rejected (count=%lu)", (unsigned long)s_auth_reject_count);
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"error\":\"unauthorized\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t portal_control_plane_send_rate_limited(httpd_req_t *req)
{
    httpd_resp_set_status(req, "429 Too Many Requests");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"error\":\"rate_limited\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

int portal_control_plane_serialize_metrics_json(char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) {
        return 0;
    }
    return snprintf(
        buf,
        buf_len,
        "{\"schemaVersion\":%u,\"authRejects\":%lu,\"otaRejects\":%lu,\"uplinkRejects\":%lu,\"mixerRejects\":%lu,"
        "\"otaRequests\":%lu,\"uplinkRequests\":%lu,\"mixerRequests\":%lu,"
        "\"otaApplyFails\":%lu,\"uplinkApplyFails\":%lu,\"mixerApplyFails\":%lu,"
        "\"badRequests\":%lu,"
        "\"rateLimit\":{\"windowMs\":%u,\"maxRequests\":%u}}",
        (unsigned)PORTAL_CONTROL_METRICS_SCHEMA_VERSION,
        (unsigned long)s_auth_reject_count,
        (unsigned long)s_ota_rate_limit.rejected_count,
        (unsigned long)s_uplink_rate_limit.rejected_count,
        (unsigned long)s_mixer_rate_limit.rejected_count,
        (unsigned long)s_ota_requests_total,
        (unsigned long)s_uplink_requests_total,
        (unsigned long)s_mixer_requests_total,
        (unsigned long)s_ota_apply_fail_count,
        (unsigned long)s_uplink_apply_fail_count,
        (unsigned long)s_mixer_apply_fail_count,
        (unsigned long)s_bad_request_count,
        (unsigned)PORTAL_CONTROL_RATE_LIMIT_WINDOW_MS,
        (unsigned)PORTAL_CONTROL_RATE_LIMIT_MAX_REQUESTS);
}
