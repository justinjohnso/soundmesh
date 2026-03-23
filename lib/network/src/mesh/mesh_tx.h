#pragma once

#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>

esp_err_t network_send_audio(const uint8_t *data, size_t len);
esp_err_t network_send_control(const uint8_t *data, size_t len);
