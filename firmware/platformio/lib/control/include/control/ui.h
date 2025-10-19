#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include "common/types.h"

// UI handle (opaque)
typedef struct ui_handle_s *ui_handle_t;

// UI configuration
typedef struct {
    i2c_master_bus_handle_t i2c_bus;
    uint8_t i2c_addr;
    bool is_tx;  // true for TX display, false for RX display
} ui_config_t;

// UI API
esp_err_t ui_init(const ui_config_t *config, ui_handle_t *out_handle);
esp_err_t ui_update_tx(ui_handle_t handle, const tx_status_t *status, display_mode_t mode);
esp_err_t ui_update_rx(ui_handle_t handle, const rx_status_t *status, display_mode_t mode);
void ui_deinit(ui_handle_t handle);
