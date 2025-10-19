#pragma once

#include <stddef.h>
#include "esp_err.h"

// Transport statistics
typedef struct {
    uint32_t packets_sent;
    uint32_t packets_received;
    uint32_t bytes_sent;
    uint32_t bytes_received;
    uint32_t errors;
} transport_stats_t;

// Transport role
typedef enum {
    TRANSPORT_ROLE_TX = 0,
    TRANSPORT_ROLE_RX = 1
} transport_role_t;

// Transport configuration
typedef struct {
    transport_role_t role;
    const char *ssid;
    const char *password;
    uint8_t channel;
    uint16_t port;
} transport_config_t;

// Transport interface (vtable pattern)
typedef struct {
    esp_err_t (*init)(const transport_config_t *cfg);
    ssize_t (*send)(const void *data, size_t len);
    ssize_t (*recv)(void *buf, size_t len, uint32_t timeout_ms);
    void (*get_stats)(transport_stats_t *stats);
    int (*get_rssi)(void);
    void (*deinit)(void);
} transport_vtable_t;

// UDP transport implementation
extern const transport_vtable_t udp_transport;
