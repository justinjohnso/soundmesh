#include "control/portal_ota.h"

#include <esp_log.h>
#include <esp_https_ota.h>
#include <esp_crt_bundle.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include "config/build.h"

static const char *TAG = "portal_ota";

typedef struct {
    bool in_progress;
    bool last_ok;
    int last_err;
    char last_url[192];
    char phase[32];
} portal_ota_state_t;

static portal_ota_state_t s_ota = {
    .in_progress = false,
    .last_ok = false,
    .last_err = 0,
    .last_url = {0},
    .phase = "idle"
};

static TaskHandle_t s_ota_task = NULL;
static char s_pending_url[192];

static const char *ota_redacted_url(void) {
    return (s_ota.last_url[0] != '\0') ? "<configured>" : "";
}

static void ota_task(void *arg) {
    (void)arg;
    strlcpy(s_ota.phase, "downloading", sizeof(s_ota.phase));

    esp_http_client_config_t http_cfg = {
        .url = s_pending_url,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_err_t ret = esp_https_ota(&ota_cfg);
    if (ret == ESP_OK) {
        s_ota.last_ok = true;
        s_ota.last_err = 0;
        strlcpy(s_ota.phase, "restarting", sizeof(s_ota.phase));
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else {
        s_ota.last_ok = false;
        s_ota.last_err = (int)ret;
        strlcpy(s_ota.phase, "failed", sizeof(s_ota.phase));
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ret));
    }

    s_ota.in_progress = false;
    s_ota_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t portal_ota_start(const char *url) {
    if (!url || strlen(url) < 8) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strncmp(url, "https://", 8) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ota.in_progress) {
        return ESP_ERR_INVALID_STATE;
    }
    if (strlen(url) >= sizeof(s_pending_url)) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_pending_url, url, sizeof(s_pending_url));
    strlcpy(s_ota.last_url, url, sizeof(s_ota.last_url));
    s_ota.in_progress = true;
    s_ota.last_ok = false;
    s_ota.last_err = 0;
    strlcpy(s_ota.phase, "queued", sizeof(s_ota.phase));

    BaseType_t ok = xTaskCreatePinnedToCore(
        ota_task, "portal_ota", PORTAL_OTA_STACK_BYTES, NULL, 4, &s_ota_task, 0);
    if (ok != pdPASS) {
        s_ota.in_progress = false;
        s_ota_task = NULL;
        strlcpy(s_ota.phase, "idle", sizeof(s_ota.phase));
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "OTA start requested");
    return ESP_OK;
}

bool portal_ota_in_progress(void) {
    return s_ota.in_progress;
}

int portal_ota_serialize_json(char *buf, size_t buf_size) {
    if (!buf || buf_size < 64) {
        return 0;
    }
    return snprintf(buf, buf_size,
                    "{\"enabled\":true,\"inProgress\":%s,\"phase\":\"%s\",\"lastOk\":%s,"
                    "\"lastErr\":%d,\"lastUrl\":\"%s\"}",
                    s_ota.in_progress ? "true" : "false",
                    s_ota.phase,
                    s_ota.last_ok ? "true" : "false",
                    s_ota.last_err,
                    ota_redacted_url());
}

void portal_ota_confirm_running_image(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        return;
    }

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    esp_err_t state_ret = esp_ota_get_state_partition(running, &state);
    if (state_ret == ESP_ERR_NOT_SUPPORTED || state_ret == ESP_ERR_NOT_FOUND) {
        return;
    }
    if (state_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed reading OTA image state: %s", esp_err_to_name(state_ret));
        return;
    }

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t confirm_ret = esp_ota_mark_app_valid_cancel_rollback();
        if (confirm_ret == ESP_OK) {
            ESP_LOGI(TAG, "OTA image confirmed valid; rollback canceled");
        } else {
            ESP_LOGE(TAG, "Failed to confirm OTA image: %s", esp_err_to_name(confirm_ret));
        }
    }
}
