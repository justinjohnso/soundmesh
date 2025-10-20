#pragma once

#include <esp_err.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

esp_err_t usb_audio_init(void);
esp_err_t usb_audio_read_frames(int16_t *frames, size_t frame_count, size_t *frames_read);
bool usb_audio_is_active(void);
