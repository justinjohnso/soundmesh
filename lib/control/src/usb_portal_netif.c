#include "control/usb_portal.h"
#include "control/portal_state.h"
#include "config/build.h"
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <lwip/esp_netif_net_stack.h>
#include <dhcpserver/dhcpserver_options.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#if defined(CONFIG_TX_BUILD) || defined(CONFIG_COMBO_BUILD)

#include "network/mesh_net.h"

/*
 * Include esp_tinyusb managed component headers.
 * PlatformIO needs -I flag in build_flags to find these.
 */
#include "tinyusb.h"
#include "tinyusb_net.h"

static const char *TAG = "portal_usb";

static bool portal_running = false;
static esp_netif_t *s_portal_netif = NULL;
static esp_netif_ip_info_t s_portal_ip;  // computed at runtime from mesh ID + node MAC

// Forward declarations for services in other files
esp_err_t portal_http_start(void);
esp_err_t portal_dns_start(void);

// ============================================================================
// USB ECM ↔ lwIP bridge callbacks
// ============================================================================

// Called by esp_tinyusb when USB host sends an Ethernet frame to us
// Must copy buffer — original is owned by TinyUSB and freed after callback returns
static esp_err_t usb_recv_callback(void *buffer, uint16_t len, void *ctx) {
    if (s_portal_netif && len > 0) {
        void *buf_copy = malloc(len);
        if (!buf_copy) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(buf_copy, buffer, len);
        esp_netif_receive(s_portal_netif, buf_copy, len, NULL);
    }
    return ESP_OK;
}

// esp_netif transmit: lwIP → USB host
static esp_err_t portal_netif_transmit(void *h, void *buffer, size_t len) {
    if (buffer && len > 0) {
        tinyusb_net_send_sync(buffer, len, NULL, pdMS_TO_TICKS(100));
    }
    return ESP_OK;
}

// esp_netif free RX buffer (we malloc'd a copy in usb_recv_callback)
static void portal_l2_free(void *h, void *buffer) {
    free(buffer);
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
    // Derive lwIP MAC from efuse base MAC (locally administered, unicast)
    uint8_t lwip_mac[6];
    esp_efuse_mac_get_default(lwip_mac);
    lwip_mac[0] = (lwip_mac[0] | 0x02) & 0xFE;

    // Compute unique /30 subnet: 10.48.<mesh_hash>.<node_base+1>
    // 3rd octet: DJB2 hash of MESH_ID string (groups nodes by network)
    // 4th octet: node MAC last byte, /30-aligned (distinguishes nodes)
    uint32_t hash = 5381;
    for (const char *p = MESH_ID; *p; p++) {
        hash = ((hash << 5) + hash) + (uint8_t)*p;
    }
    uint8_t mesh_octet = (uint8_t)(hash & 0xFF);
    uint8_t node_base  = lwip_mac[5] & 0xFC;  // /30 aligned: 0,4,8,...,252

    IP4_ADDR(&s_portal_ip.ip,      10, 48, mesh_octet, node_base + 1);
    IP4_ADDR(&s_portal_ip.netmask, 255, 255, 255, 252);
    IP4_ADDR(&s_portal_ip.gw,      10, 48, mesh_octet, node_base + 1);

    // 1) Inherent config — DHCP server + auto-up (similar to WiFi AP)
    esp_netif_inherent_config_t base_cfg = {
        .flags = (esp_netif_flags_t)(ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP),
        .ip_info = &s_portal_ip,
        .if_key = "USB_NCM_DEF",
        .if_desc = "usb-ncm-portal",
        .route_prio = 10,
    };

    // 2) Driver config — point to static transmit and free functions
    esp_netif_driver_ifconfig_t driver_cfg = {
        .handle = (void *)1,  // USB-NCM is a singleton; must be non-NULL
        .transmit = portal_netif_transmit,
        .driver_free_rx_buffer = portal_l2_free,
    };

    // 3) lwIP netstack — USB-NCM is Ethernet from lwIP's perspective
    struct esp_netif_netstack_config lwip_netif_config = {
        .lwip = {
            .init_fn = ethernetif_init,
            .input_fn = ethernetif_input,
        }
    };

    esp_netif_config_t cfg = {
        .base = &base_cfg,
        .driver = &driver_cfg,
        .stack = &lwip_netif_config,
    };

    s_portal_netif = esp_netif_new(&cfg);
    if (!s_portal_netif) {
        ESP_LOGE(TAG, "Failed to create esp_netif");
        return ESP_FAIL;
    }

    // Set MAC after creation (not via base_cfg.mac)
    esp_netif_set_mac(s_portal_netif, lwip_mac);

    // Set minimum DHCP lease time
    uint32_t lease_opt = 1;
    esp_netif_dhcps_option(s_portal_netif, ESP_NETIF_OP_SET,
                           IP_ADDRESS_LEASE_TIME, &lease_opt, sizeof(lease_opt));

    // Bring interface up (driver already started by tinyusb_net_init)
    esp_netif_action_start(s_portal_netif, 0, 0, 0);

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
    esp_err_t ret = portal_state_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Portal state init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register heartbeat callback to collect mesh node data
    ret = network_register_heartbeat_callback(on_heartbeat_received);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Heartbeat callback register failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Initialize TCP/IP stack and event loop (safe to call multiple times)
    ret = esp_netif_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    esp_err_t evt_err = esp_event_loop_create_default();
    if (evt_err != ESP_OK && evt_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Event loop create failed: %s", esp_err_to_name(evt_err));
        return evt_err;
    }

    // Install TinyUSB driver
    tinyusb_config_t tusb_cfg = { 0 };
    tusb_cfg.external_phy = false;
    ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "TinyUSB driver installed");

    // Initialize USB NCM network class
    tinyusb_net_config_t net_config = {
        .on_recv_callback = usb_recv_callback,
    };
    // Derive USB MAC from chip WiFi STA MAC, set locally administered bit
    esp_read_mac(net_config.mac_addr, ESP_MAC_WIFI_STA);
    net_config.mac_addr[0] |= 0x02;
    net_config.mac_addr[0] &= ~0x01;
    ESP_LOGI(TAG, "USB ECM MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             net_config.mac_addr[0], net_config.mac_addr[1],
             net_config.mac_addr[2], net_config.mac_addr[3],
             net_config.mac_addr[4], net_config.mac_addr[5]);
    ret = tinyusb_net_init(TINYUSB_USBDEV_0, &net_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB net init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "USB NCM network class initialized");

    // Create esp_netif with static IP and DHCP server
    ret = portal_netif_setup();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Portal netif setup failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Start HTTP server
    ret = portal_http_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "HTTP server started");

    // Start DNS catch-all server
    ret = portal_dns_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "DNS server start failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "DNS server started");

    portal_running = true;
    ESP_LOGI(TAG, "Portal fully initialized — heap: %lu bytes remaining",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));

    return ESP_OK;
}

bool portal_is_running(void) {
    return portal_running;
}

const esp_netif_ip_info_t *portal_get_ip_info(void) {
    return &s_portal_ip;
}

#else /* RX build — portal not supported */

esp_err_t portal_init(void) {
    return ESP_ERR_NOT_SUPPORTED;
}

bool portal_is_running(void) {
    return false;
}

const esp_netif_ip_info_t *portal_get_ip_info(void) {
    return NULL;
}

#endif /* CONFIG_TX_BUILD || CONFIG_COMBO_BUILD */
