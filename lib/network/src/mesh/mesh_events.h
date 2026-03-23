#pragma once

#include <esp_event_base.h>

void mesh_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
