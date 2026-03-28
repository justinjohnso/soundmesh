#include <stdint.h>
#include <string.h>

#include <unity.h>

#include "control/portal_control_plane.h"
#include "config/build.h"

int64_t esp_timer_get_time(void)
{
    return 0;
}

size_t httpd_req_get_hdr_value_len(httpd_req_t *req, const char *field)
{
    (void)req;
    (void)field;
    return 0;
}

esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req, const char *field, char *val, size_t val_size)
{
    (void)req;
    (void)field;
    (void)val;
    (void)val_size;
    return ESP_ERR_INVALID_ARG;
}

size_t httpd_req_get_url_query_len(httpd_req_t *req)
{
    (void)req;
    return 0;
}

esp_err_t httpd_req_get_url_query_str(httpd_req_t *req, char *buf, size_t buf_len)
{
    (void)req;
    (void)buf;
    (void)buf_len;
    return ESP_ERR_INVALID_ARG;
}

esp_err_t httpd_query_key_value(const char *qry, const char *key, char *val, size_t val_size)
{
    (void)qry;
    (void)key;
    (void)val;
    (void)val_size;
    return ESP_ERR_INVALID_ARG;
}

esp_err_t httpd_resp_set_status(httpd_req_t *req, const char *status)
{
    (void)req;
    (void)status;
    return ESP_OK;
}

esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type)
{
    (void)req;
    (void)type;
    return ESP_OK;
}

esp_err_t httpd_resp_send(httpd_req_t *req, const char *buf, ssize_t buf_len)
{
    (void)req;
    (void)buf;
    (void)buf_len;
    return ESP_OK;
}

#include "../../../lib/control/src/portal_control_plane.c"

void test_metrics_json_baseline_exceeds_legacy_256_buffer(void)
{
    portal_control_plane_reset();
    char json[PORTAL_CONTROL_METRICS_JSON_BUF_SIZE];

    int len = portal_control_plane_serialize_metrics_json(json, sizeof(json));
    TEST_ASSERT_TRUE(len >= 256);
    TEST_ASSERT_TRUE(len < (int)sizeof(json));
}

void test_metrics_json_serializes_with_configured_buffer(void)
{
    portal_control_plane_reset();
    portal_control_plane_record_request(PORTAL_CONTROL_ENDPOINT_OTA);
    portal_control_plane_record_request(PORTAL_CONTROL_ENDPOINT_UPLINK);
    portal_control_plane_record_request(PORTAL_CONTROL_ENDPOINT_MIXER);
    portal_control_plane_record_apply_failure(PORTAL_CONTROL_ENDPOINT_OTA);
    portal_control_plane_record_bad_request();

    char json[PORTAL_CONTROL_METRICS_JSON_BUF_SIZE];
    int len = portal_control_plane_serialize_metrics_json(json, sizeof(json));

    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_TRUE(len < (int)sizeof(json));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"schemaVersion\":"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"authRejects\":"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"rateLimit\":{\"windowMs\":"));
}

void test_metrics_json_reports_truncation_with_small_buffer(void)
{
    portal_control_plane_reset();
    char json[128];

    int len = portal_control_plane_serialize_metrics_json(json, sizeof(json));
    TEST_ASSERT_TRUE(len >= (int)sizeof(json));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_metrics_json_baseline_exceeds_legacy_256_buffer);
    RUN_TEST(test_metrics_json_serializes_with_configured_buffer);
    RUN_TEST(test_metrics_json_reports_truncation_with_small_buffer);
    return UNITY_END();
}
