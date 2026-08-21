// WiFi via ESP-Hosted (C6 coprocessor over SDIO), standalone module.
// Depends only on managed components espressif__esp_hosted + espressif__esp_wifi_remote.

#include "wifi_app.h"

#include "otool_tab5_component.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <cstdio>
#include <cstring>

static const char *TAG = "wifi_app";

static constexpr EventBits_t WIFI_CONNECTED_BIT = BIT0;

static EventGroupHandle_t s_wifi_events = nullptr;
static int s_wifi_retries = 0;
static void *s_tab5_comp = nullptr;

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA start, connecting to %s", CONFIG_OTOOL_WIFI_SSID);
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_retries < CONFIG_OTOOL_WIFI_MAX_RETRY) {
            s_wifi_retries++;
            ESP_LOGW(TAG, "disconnected (retry %d/%d), reconnecting...", s_wifi_retries,
                     CONFIG_OTOOL_WIFI_MAX_RETRY);
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "wifi give up after %d retries", s_wifi_retries);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_retries = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_app_start(void *tab5_comp)
{
    s_tab5_comp = tab5_comp;
    s_wifi_events = xEventGroupCreate();
    if (s_wifi_events == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    /* C6 模块上电：WLAN_PWR_EN 由 IO 扩展器（ADDR_HIGH 0x44 P0）控制。
     * 先上电并等待 1s 稳定，再初始化 ESP-Hosted（SDIO 链路）。 */
    m5::tab5::otool_tab5_component *comp =
        (m5::tab5::otool_tab5_component *)s_tab5_comp;
    if (comp != nullptr) {
        esp_err_t perr = comp->wlan_power(true);
        if (perr != ESP_OK) {
            ESP_LOGW(TAG, "wlan_power(true) failed: %s", esp_err_to_name(perr));
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "ESP-Hosted init (C6 SDIO)...");

    /* 基础网络栈（nvs_flash_init 已在 app_main 完成） */
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default: %s", esp_err_to_name(err));
        return err;
    }
    esp_netif_create_default_wifi_sta();

    /* esp_wifi_init 由 esp_wifi_remote 路由到 C6（esp_hosted SDIO 链路） */
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    wcfg.dynamic_rx_buf_num = 48;
    wcfg.dynamic_tx_buf_num = 48;
    wcfg.static_rx_buf_num = 16;
    err = esp_wifi_init(&wcfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_storage: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode: %s", esp_err_to_name(err));
        return err;
    }

    wifi_country_t country = {};
    country.cc[0] = 'C';
    country.cc[1] = 'N';
    country.cc[2] = 0;
    country.schan = 1;
    country.nchan = 13;
    country.max_tx_power = 20;
    country.policy = WIFI_COUNTRY_POLICY_AUTO;
    err = esp_wifi_set_country(&country);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_country: %s", esp_err_to_name(err));
        return err;
    }

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr);

    wifi_config_t wifi_config = {};
    snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", CONFIG_OTOOL_WIFI_SSID);
    snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s",
             CONFIG_OTOOL_WIFI_PASSWORD);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_MODE) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "ESP-Hosted init done, STA start");
    return ESP_OK;
}

bool wifi_app_is_connected(void)
{
    if (s_wifi_events == nullptr) {
        return false;
    }
    return (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) != 0;
}

esp_err_t wifi_app_wait_connected(uint32_t timeout_ms)
{
    if (s_wifi_events == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                                           timeout_ms == 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t wifi_app_reconnect(void)
{
    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "disconnect: %s", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    err = esp_wifi_connect();
    ESP_LOGI(TAG, "reconnect -> %s", esp_err_to_name(err));
    return err;
}
