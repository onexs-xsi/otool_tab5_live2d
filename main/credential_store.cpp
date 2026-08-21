// Runtime credential store (NVS). 仓库/固件默认不含真实凭证（WP0）。

#include "credential_store.h"

#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

#include <cstdio>
#include <cstring>

static const char *TAG = "credential";

#define CRED_NVS_NAMESPACE "otool_cred"
#define CRED_MAX_VALUE 256

static const char *NVS_KEY_WIFI_SSID = "wifi_ssid";
static const char *NVS_KEY_WIFI_PASS = "wifi_pass";
static const char *NVS_KEY_LLM_KEY = "llm_key";

/* 静态缓存：读取一次后常驻（值为自有的深拷贝，destroy 时不公开清理） */
static char s_wifi_ssid[CRED_MAX_VALUE] = { 0 };
static char s_wifi_pass[CRED_MAX_VALUE] = { 0 };
static char s_llm_key[CRED_MAX_VALUE] = { 0 };
static bool s_loaded = false;

static void cache_load(const char *nvs_key, char *cache, size_t cap, const char *fallback)
{
    nvs_handle_t h;
    if (nvs_open(CRED_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = cap;
        if (nvs_get_str(h, nvs_key, cache, &len) != ESP_OK) {
            cache[0] = '\0';
        }
        nvs_close(h);
    }
    if (cache[0] == '\0' && fallback != nullptr) {
        snprintf(cache, cap, "%s", fallback);
    }
}

esp_err_t credential_store_init(void)
{
    if (s_loaded) {
        return ESP_OK;
    }
    cache_load(NVS_KEY_WIFI_SSID, s_wifi_ssid, sizeof(s_wifi_ssid), CONFIG_OTOOL_WIFI_SSID);
    cache_load(NVS_KEY_WIFI_PASS, s_wifi_pass, sizeof(s_wifi_pass), nullptr);
    cache_load(NVS_KEY_LLM_KEY, s_llm_key, sizeof(s_llm_key), nullptr);
    s_loaded = true;
    ESP_LOGI(TAG, "credentials loaded (wifi_ssid=%s wifi_pass=%s llm_key=%s)",
             s_wifi_ssid[0] ? s_wifi_ssid : "<none>",
             s_wifi_pass[0] ? "<set>" : "<none>",
             s_llm_key[0] ? "<set>" : "<none>");
    return ESP_OK;
}

esp_err_t credential_store_get(const char *name, char *out, size_t cap)
{
    if (name == nullptr || out == nullptr || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(CRED_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t len = cap;
    err = nvs_get_str(h, name, out, &len);
    nvs_close(h);
    return err;
}

esp_err_t credential_store_set(const char *name, const char *value)
{
    if (name == nullptr || value == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(value) >= CRED_MAX_VALUE) {
        return ESP_ERR_INVALID_SIZE;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(CRED_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, name, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        s_loaded = false; /* 下次读取时重新加载缓存 */
    }
    return err;
}

esp_err_t credential_store_erase(const char *name)
{
    if (name == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(CRED_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(h, name);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        s_loaded = false;
    }
    return err;
}

const char *credential_wifi_ssid(void)
{
    if (!s_loaded) {
        credential_store_init();
    }
    return s_wifi_ssid;
}

const char *credential_wifi_password(void)
{
    if (!s_loaded) {
        credential_store_init();
    }
    return s_wifi_pass;
}

const char *credential_llm_key(void)
{
    if (!s_loaded) {
        credential_store_init();
    }
    return s_llm_key;
}
