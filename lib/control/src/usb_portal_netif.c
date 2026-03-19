#include "control/usb_portal.h"
#include "control/portal_state.h"
#include "config/build.h"
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#if defined(CONFIG_TX_BUILD) || defined(CONFIG_COMBO_BUILD)

#include "network/mesh_net.h"

/*
 * Forward-declare esp_tinyusb symbols we need.
 * PlatformIO lib/ code can't include managed component headers directly,
 * so we declare only the functions/types we call.
 */

// From tinyusb.h (esp_tinyusb managed component)
typedef struct {
    const void *device_descriptor;
    const char **string_descriptor;
    const void *configuration_descriptor;
    bool external_phy;
    bool self_powered;
    int vbus_monitor_io;
} tinyusb_config_t;
extern esp_err_t tinyusb_driver_install(const tinyusb_config_t *config);

// From tinyusb_net.h (esp_tinyusb managed component)
typedef enum {
    TINYUSB_USBDEV_0,
} tinyusb_usbdev_t;

typedef esp_err_t (*tinyusb_net_recv_cb_t)(void *buffer, uint16_t len, void *ctx);
typedef void (*tinyusb_net_free_tx_cb_t)(void *buffer, void *ctx);

typedef struct {
    uint8_t mac_addr[6];
    tinyusb_net_recv_cb_t on_recv_callback;
    tinyusb_net_free_tx_cb_t free_tx_buffer;
    void *user_context;
} tinyusb_net_config_t;

extern esp_err_t tinyusb_net_init(tinyusb_usbdev_t usb_dev, const tinyusb_net_config_t *cfg);
extern esp_err_t tinyusb_net_send_sync(const void *buffer, uint16_t len, void *buff_free_arg, uint32_t timeout);

static const char *TAG = "portal_usb";

static bool portal_running = false;
static esp_netif_t *s_portal_netif = NULL;

// Forward declarations for services in other files
esp_err_t portal_http_start(void);
esp_err_t portal_dns_start(void);

// ============================================================================
// USB ECM ↔ lwIP bridge callbacks
// ============================================================================

// Called by esp_tinyusb when USB host sends an Ethernet frame to us
static esp_err_t usb_recv_callback(void *buffer, uint16_t len, void *ctx) {
    if (s_portal_netif && len > 0) {
        esp_netif_receive(s_portal_netif, buffer, len, NULL);
    }
    return ESP_OK;
}

// Called by esp_tinyusb when TX buffer can be freed
static void usb_free_tx_buffer(void *buffer, void *ctx) {
    // tinyusb_net_send_sync copies internally; nothing to free
}

// esp_netif transmit: lwIP → USB host
static esp_err_t portal_netif_transmit(void *h, void *buffer, size_t len) {
    if (buffer && len > 0) {
        tinyusb_net_send_sync(buffer, len, NULL, pdMS_TO_TICKS(100));
    }
    return ESP_OK;
}

// esp_netif free RX buffer
static void portal_l2_free(void *h, void *buffer) {
    // Buffer owned by TinyUSB; freed after recv callback returns
}

// ============================================================================
// Heartbeat callback
// ============================================================================

static void on_heartbeat_received(const uint8_t *sender_mac, const mesh_heartbeat_t *hb) {
    portal_state_update_from_heartbeat(sender_mac, hb);
}

// ============================================================================
// Create esp_netif with static IP + DHCP server
// ============================================================================

static esp_err_t portal_netif_setup(void) {
    // Device-side MAC (locally administered unicast)
    uint8_t dev_mac[6] = {0x02, 0x02, 0x84, 0x6A, 0x96, 0x02};

    // Static IP: 10.48.0.1/24
    static const esp_netif_ip_info_t portal_ip = {
        .ip      = { .addr = ESP_IP4TOADDR(10, 48, 0, 1) },
        .netmask = { .addr = ESP_IP4TOADDR(255, 255, 255, 0) },
        .gw      = { .addr = ESP_IP4TOADDR(10, 48, 0, 1) },
    };

    // Inherent config modeled on WiFi AP (DHCP server, auto-up)
    esp_netif_inherent_config_t base_cfg = {
        .flags = (esp_netif_flags_t)(ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP),
        .ip_info = &portal_ip,
        .if_key = "USB_ECM_DEF",
        .if_desc = "usb-ecm-portal",
        .route_prio = 5,
    };
    memcpy(base_cfg.mac, dev_mac, 6);

    // Driver config
    esp_netif_driver_ifconfig_t driver_cfg = {
        .handle = (void *)1,  // USB-ECM is a singleton; must be non-NULL
        .transmit = portal_netif_transmit,
        .driver_free_rx_buffer = portal_l2_free,
    };

    // lwIP Ethernet netstack
    extern const struct esp_netif_netstack_config *_g_esp_netif_netstack_default_eth;
    esp_netif_config_t cfg = {
        .base = &base_cfg,
        .driver = &driver_cfg,
        .stack = _g_esp_netif_netstack_default_eth,
    };

    s_portal_netif = esp_netif_new(&cfg);
    if (!s_portal_netif) {
        ESP_LOGE(TAG, "Failed to create esp_netif");
        return ESP_FAIL;
    }

    // Stop DHCP client (we're a server)
    esp_netif_dhcpc_stop(s_portal_netif);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_portal_netif, &portal_ip));

    // Start DHCP server
    esp_netif_dhcps_stop(s_portal_netif);
    ESP_ERROR_CHECK(esp_netif_dhcps_start(s_portal_netif));

    // Bring interface up
    esp_netif_action_start(s_portal_netif, NULL, 0, NULL);

    esp_netif_ip_info_t info;
    esp_netif_get_ip_info(s_portal_netif, &info);
    ESP_LOGI(TAG, "USB netif up: IP=" IPSTR " Mask=" IPSTR " GW=" IPSTR,
             IP2STR(&info.ip), IP2STR(&info.netmask), IP2STR(&info.gw));

    return ESP_OK;
}

// ============================================================================
// portal_init — main entry point
// ============================================================================

esp_err_t portal_init(void) {
    if (portal_running) {
        ESP_LOGW(TAG, "Portal already running");
        return ESP_OK;
    }

    uint32_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "Portal init: free heap = %lu bytes", (unsigned long)free_heap);
    if (free_heap < PORTAL_MIN_FREE_HEAP) {
        ESP_LOGW(TAG, "Insufficient heap for portal (%lu < %d), skipping",
                 (unsigned long)free_heap, PORTAL_MIN_FREE_HEAP);
        return ESP_ERR_NO_MEM;
    }

    // Initialize portal state tracking
    ESP_ERROR_CHECK(portal_state_init());

    // Register heartbeat callback to collect mesh node data
    ESP_ERROR_CHECK(network_register_heartbeat_callback(on_heartbeat_received));

    // Initialize TCP/IP stack and event loop (safe to call multiple times)
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t evt_err = esp_event_loop_create_default();
    if (evt_err != ESP_OK && evt_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(evt_err);
    }

    // Install TinyUSB driver
    tinyusb_config_t tusb_cfg = { 0 };
    tusb_cfg.external_phy = false;
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    ESP_LOGI(TAG, "TinyUSB driver installed");

    // Initialize USB ECM network class
    tinyusb_net_config_t net_config = {
        .on_recv_callback = usb_recv_callback,
        .free_tx_buffer = usb_free_tx_buffer,
        .user_context = NULL,
    };
    // Derive USB MAC from chip WiFi STA MAC, set locally administered bit
    esp_read_mac(net_config.mac_addr, ESP_MAC_WIFI_STA);
    net_config.mac_addr[0] |= 0x02;
    net_config.mac_addr[0] &= ~0x01;
    ESP_LOGI(TAG, "USB ECM MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             net_config.mac_addr[0], net_config.mac_addr[1],
             net_config.mac_addr[2], net_config.mac_addr[3],
             net_config.mac_addr[4], net_config.mac_addr[5]);
    ESP_ERROR_CHECK(tinyusb_net_init(TINYUSB_USBDEV_0, &net_config));
    ESP_LOGI(TAG, "USB ECM network class initialized");

    // Create esp_netif with static IP and DHCP server
    ESP_ERROR_CHECK(portal_netif_setup());

    // Start HTTP server
    ESP_ERROR_CHECK(portal_http_start());
    ESP_LOGI(TAG, "HTTP server started");

    // Start DNS catch-all server
    ESP_ERROR_CHECK(portal_dns_start());
    ESP_LOGI(TAG, "DNS server started");

    portal_running = true;
    ESP_LOGI(TAG, "Portal fully initialized — heap: %lu bytes remaining",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));

    return ESP_OK;
}

bool portal_is_running(void) {
    return portal_running;
}

#else /* RX build — portal not supported */

esp_err_t portal_init(void) {
    return ESP_ERR_NOT_SUPPORTED;
}

bool portal_is_running(void) {
    return false;
}

#endif /* CONFIG_TX_BUILD || CONFIG_COMBO_BUILD */
