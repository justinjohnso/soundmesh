#pragma once

#include "esp_err.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct httpd_req httpd_req_t;

size_t httpd_req_get_hdr_value_len(httpd_req_t *req, const char *field);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req, const char *field, char *val, size_t val_size);
size_t httpd_req_get_url_query_len(httpd_req_t *req);
esp_err_t httpd_req_get_url_query_str(httpd_req_t *req, char *buf, size_t buf_len);
esp_err_t httpd_query_key_value(const char *qry, const char *key, char *val, size_t val_size);
esp_err_t httpd_resp_set_status(httpd_req_t *req, const char *status);
esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type);
esp_err_t httpd_resp_send(httpd_req_t *req, const char *buf, ssize_t buf_len);

#define HTTPD_RESP_USE_STRLEN ((ssize_t)-1)

#ifdef __cplusplus
}
#endif
