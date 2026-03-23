#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t portal_ota_start(const char *url);
bool portal_ota_in_progress(void);
int portal_ota_serialize_json(char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

