#include "mesh/mesh_init.h"
#include "mesh/mesh_state.h"
#include "mesh/mesh_identity.h"
#include "mesh/mesh_events.h"
#include "mesh/mesh_rx.h"
#include "mesh/mesh_heartbeat.h"
#include "config/build.h"
#include "network/mesh_net.h"
#include <esp_event.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_mesh.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <string.h>

static const char *TAG = "network_mesh";

esp_err_t network_init_mesh(void) {
    ESP_LOGI(TAG, "Initializing ESP-WIFI-MESH");

#if defined(CONFIG_TX_BUILD) || defined(CONFIG_COMBO_BUILD)
    my_node_role = NODE_ROLE_TX;
    ESP_LOGI(TAG, "Node role: TX/COMBO (root preference enabled)");
#else
    my_node_role = NODE_ROLE_RX;
    ESP_LOGI(TAG, "Node role: RX");
#endif

    esp_read_mac(my_sta_mac, ESP_MAC_WIFI_STA);
    my_stream_id = my_sta_mac[0] ^ my_sta_mac[1] ^ my_sta_mac[2] ^
                   my_sta_mac[3] ^ my_sta_mac[4] ^ my_sta_mac[5];
    derive_src_id(my_sta_mac, g_src_id);
    ESP_LOGI(TAG, "Node MAC: " MACSTR ", stream_id=0x%02X, src_id=%s",
             MAC2STR(my_sta_mac), my_stream_id, g_src_id);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(esp_netif_create_default_wifi_mesh_netifs(NULL, NULL));

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(WIFI_TX_POWER_QDBM));
    ESP_LOGI(TAG, "WiFi TX power set to %.1f dBm", ((float)WIFI_TX_POWER_QDBM) / 4.0f);

    ESP_ERROR_CHECK(esp_mesh_init());

    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID, &mesh_event_handler, NULL));

    esp_netif_t *mesh_netif = esp_netif_get_handle_from_ifkey("WIFI_MESH_ROOT");
    if (mesh_netif == NULL) {
        mesh_netif = esp_netif_get_handle_from_ifkey("WIFI_MESH");
    }
    (void)mesh_netif;

    mesh_cfg_t mesh_config = MESH_INIT_CONFIG_DEFAULT();

    uint8_t mesh_id_bytes[6];
    mesh_id_from_string(MESH_ID, mesh_id_bytes);
    memcpy((uint8_t *)&mesh_config.mesh_id, mesh_id_bytes, 6);

    ESP_LOGI(TAG, "Mesh ID: %02X:%02X:%02X:%02X:%02X:%02X (\"%s\")",
             mesh_id_bytes[0], mesh_id_bytes[1], mesh_id_bytes[2],
             mesh_id_bytes[3], mesh_id_bytes[4], mesh_id_bytes[5], MESH_ID);
    ESP_LOGI(TAG, "Mesh config: role=%s channel=%d fixed_root=1 src_id=%s",
             my_node_role == NODE_ROLE_TX ? "TX/COMBO" : "RX", MESH_CHANNEL, g_src_id);

    mesh_config.channel = MESH_CHANNEL;

    memset(&mesh_config.router, 0, sizeof(mesh_config.router));
    strcpy((char *)mesh_config.router.ssid, "MESHNET_DISABLED");
    mesh_config.router.ssid_len = strlen("MESHNET_DISABLED");
    mesh_config.router.password[0] = '\0';
    mesh_config.router.allow_router_switch = false;
    memset(mesh_config.router.bssid, 0, 6);

    strcpy((char *)mesh_config.mesh_ap.password, MESH_PASSWORD);
    mesh_config.mesh_ap.max_connection = 10;
    mesh_config.mesh_ap.nonmesh_max_connection = 0;

    ESP_ERROR_CHECK(esp_mesh_set_config(&mesh_config));

    ESP_ERROR_CHECK(esp_mesh_set_self_organized(true, my_node_role == NODE_ROLE_RX));
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(MESH_MAX_LAYER));
    ESP_ERROR_CHECK(esp_mesh_set_xon_qsize(MESH_XON_QSIZE));
    ESP_ERROR_CHECK(esp_mesh_set_ap_assoc_expire(MESH_AP_ASSOC_EXPIRE_S));

    ESP_LOGI(TAG, "Mesh tuning: max_layer=%u xon_qsize=%u ap_assoc_expire=%us",
             (unsigned)MESH_MAX_LAYER, (unsigned)MESH_XON_QSIZE, (unsigned)MESH_AP_ASSOC_EXPIRE_S);

    if (my_node_role == NODE_ROLE_TX) {
        ESP_ERROR_CHECK(esp_mesh_set_type(MESH_ROOT));
        ESP_LOGI(TAG, "Designated root: TX/COMBO node set as MESH_ROOT");
    }
    ESP_ERROR_CHECK(esp_mesh_fix_root(true));

    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    xTaskCreate(mesh_rx_task, "mesh_rx", MESH_RX_TASK_STACK, NULL, MESH_RX_TASK_PRIO, NULL);
    xTaskCreate(mesh_heartbeat_task, "mesh_hb", HEARTBEAT_TASK_STACK, NULL, HEARTBEAT_TASK_PRIO, &heartbeat_task_handle);

    network_register_startup_notification(heartbeat_task_handle);

    ESP_ERROR_CHECK(esp_mesh_start());

    if (my_node_role == NODE_ROLE_TX) {
        esp_wifi_scan_stop();
        ESP_LOGI(TAG, "TX/COMBO: stopped startup scanning");
    }

    if (my_node_role == NODE_ROLE_RX) {
        esp_err_t grp_err = esp_mesh_set_group_id((mesh_addr_t *)&audio_multicast_group, 1);
        if (grp_err == ESP_OK) {
            ESP_LOGI(TAG, "RX: subscribed to audio multicast group");
        } else {
            ESP_LOGW(TAG, "RX: failed to subscribe to multicast group: %s", esp_err_to_name(grp_err));
        }
    }

    ESP_LOGI(TAG, "Mesh initialized: ID=%s, Channel=%d", MESH_ID, MESH_CHANNEL);

    return ESP_OK;
}
