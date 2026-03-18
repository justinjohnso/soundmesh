#include "control/usb_portal.h"
#include "control/portal_state.h"
#include "config/build.h"
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "network/mesh_net.h"

static const char *TAG = "portal_usb";

static bool portal_running = false;

// Forward declarations
esp_err_t portal_http_start(void);
esp_err_t portal_dns_start(void);

static void on_heartbeat_received(const uint8_t *sender_mac, const mesh_heartbeat_t *hb) {
    portal_state_update_from_heartbeat(sender_mac, hb);
}

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
    
    // Initialize portal state
    ESP_ERROR_CHECK(portal_state_init());
    
    // Register heartbeat callback to collect mesh node data
    ESP_ERROR_CHECK(network_register_heartbeat_callback(on_heartbeat_received));
    
    // USB CDC-NCM networking requires esp_tinyusb component with NCM support.
    // This is available in ESP-IDF 5.2+ or as a managed component.
    // For now, portal state collection is active but USB networking is deferred.
    // The /api/status endpoint will be available once USB NCM is integrated.
    ESP_LOGW(TAG, "USB CDC-NCM networking not yet available in this ESP-IDF version");
    ESP_LOGI(TAG, "Portal state collection active (heartbeat callback registered)");
    
    portal_running = true;
    ESP_LOGI(TAG, "Portal initialized (state only, no USB netif) - heap: %lu bytes remaining",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    
    return ESP_OK;
}

bool portal_is_running(void) {
    return portal_running;
}
