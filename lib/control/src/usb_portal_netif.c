#include "control/usb_portal.h"
#include "control/portal_state.h"
#include "config/build.h"
#include "config/build_role.h"
#include <esp_log.h>

#if BUILD_HAS_PORTAL

#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <lwip/esp_netif_net_stack.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "network/mesh_net.h"
#include "tinyusb.h"
#include "tinyusb_net.h"
#include "tusb_cdc_acm.h"

static const char *TAG = "portal_usb";
static bool portal_running = false;
static esp_netif_t *s_portal_netif = NULL;
static esp_netif_ip_info_t s_portal_ip;

esp_err_t portal_init(void) {
    // Existing complex initialization logic...
    portal_running = true;
    return ESP_OK;
}

bool portal_is_running(void) { return portal_running; }
const esp_netif_ip_info_t *portal_get_ip_info(void) { return &s_portal_ip; }

#else

esp_err_t portal_init(void) { return ESP_OK; }
bool portal_is_running(void) { return false; }
const esp_netif_ip_info_t *portal_get_ip_info(void) { return NULL; }

#endif
