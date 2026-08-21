#include "otool_esp_hosted_fw_update.h"

extern "C" {
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
}

static const char *TAG = "fw_update_init";
static bool s_fw_update_inited = false;

static esp_err_t otool_esp_hosted_fw_update_basic_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    return ESP_OK;
}

extern "C" esp_err_t otool_esp_hosted_fw_update_minimal_init(void)
{
    if (s_fw_update_inited) {
        return ESP_OK;
    }

    esp_err_t ret = otool_esp_hosted_fw_update_basic_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "basic init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    (void) esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.dynamic_rx_buf_num = 48;
    cfg.dynamic_tx_buf_num = 48;
    cfg.static_rx_buf_num = 16;

    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_storage failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(ret));
        return ret;
    }

    wifi_country_t country = {};
    country.cc[0] = 'C';
    country.cc[1] = 'N';
    country.cc[2] = 0;
    country.schan = 1;
    country.nchan = 13;
    country.max_tx_power = 20;
    country.policy = WIFI_COUNTRY_POLICY_AUTO;

    ret = esp_wifi_set_country(&country);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_country failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_MODE) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_fw_update_inited = true;
    ESP_LOGI(TAG, "Minimal OTA init done");
    return ESP_OK;
}

