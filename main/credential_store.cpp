// Effective credentials: local sdkconfig first, optional NVS fallback second.

#include "credential_store.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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

/* 静态缓存：只在 mutex 下读取/覆盖；worker 只能取得临时副本。 */
static char s_wifi_ssid[CRED_MAX_VALUE] = { 0 };
static char s_wifi_pass[CRED_MAX_VALUE] = { 0 };
static char s_llm_key[CRED_MAX_VALUE] = { 0 };
static bool s_loaded = false;
static bool s_init_logged = false;
static SemaphoreHandle_t s_lock = nullptr;

static void cache_zero(char *value, size_t len)
{
    volatile char *p = value;
    while (len-- > 0) {
        *p++ = '\0';
    }
}

static void cached_value_invalidate(const char *name)
{
    if (strcmp(name, NVS_KEY_WIFI_SSID) == 0) {
        cache_zero(s_wifi_ssid, sizeof(s_wifi_ssid));
    } else if (strcmp(name, NVS_KEY_WIFI_PASS) == 0) {
        cache_zero(s_wifi_pass, sizeof(s_wifi_pass));
    } else if (strcmp(name, NVS_KEY_LLM_KEY) == 0) {
        cache_zero(s_llm_key, sizeof(s_llm_key));
    } else {
        return;
    }
    s_loaded = false;
}

static bool credential_name_valid(const char *name)
{
    return name != nullptr &&
           (strcmp(name, NVS_KEY_WIFI_SSID) == 0 || strcmp(name, NVS_KEY_WIFI_PASS) == 0 ||
            strcmp(name, NVS_KEY_LLM_KEY) == 0);
}

static void cache_load(const char *nvs_key, char *cache, size_t cap, const char *configured)
{
    cache[0] = '\0';
    if (configured != nullptr && configured[0] != '\0') {
        int written = snprintf(cache, cap, "%s", configured);
        if (written < 0 || (size_t)written >= cap) {
            cache_zero(cache, cap);
            ESP_LOGE(TAG, "%s from sdkconfig exceeds the credential buffer", nvs_key);
        }
        return;
    }

    nvs_handle_t h;
    if (nvs_open(CRED_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = cap;
        if (nvs_get_str(h, nvs_key, cache, &len) != ESP_OK) {
            cache[0] = '\0';
        }
        nvs_close(h);
    }
}

static void cache_load_all(void)
{
    cache_load(NVS_KEY_WIFI_SSID, s_wifi_ssid, sizeof(s_wifi_ssid), CONFIG_OTOOL_WIFI_SSID);
    cache_load(NVS_KEY_WIFI_PASS, s_wifi_pass, sizeof(s_wifi_pass), CONFIG_OTOOL_WIFI_PASSWORD);
    cache_load(NVS_KEY_LLM_KEY, s_llm_key, sizeof(s_llm_key), CONFIG_OTOOL_LLM_API_KEY);
    s_loaded = true;
}

esp_err_t credential_store_init(void)
{
    if (s_lock == nullptr) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_loaded) {
        cache_load_all();
    }
    bool has_ssid = s_wifi_ssid[0] != '\0';
    bool has_pass = s_wifi_pass[0] != '\0';
    bool has_key = s_llm_key[0] != '\0';
    bool should_log = !s_init_logged;
    s_init_logged = true;
    xSemaphoreGive(s_lock);
    if (!should_log) {
        return ESP_OK;
    }
#if !defined(CONFIG_NVS_ENCRYPTION) || !CONFIG_NVS_ENCRYPTION
    ESP_LOGW(TAG, "NVS encryption is disabled; any credential fallback stored there is plaintext at rest");
#endif
    ESP_LOGI(TAG, "credentials loaded (wifi_ssid=%s wifi_pass=%s llm_key=%s)",
             has_ssid ? "<set>" : "<none>", has_pass ? "<set>" : "<none>",
             has_key ? "<set>" : "<none>");
    return ESP_OK;
}

esp_err_t credential_store_get(const char *name, char *out, size_t cap)
{
    if (!credential_name_valid(name) || out == nullptr || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = credential_store_init();
    if (err != ESP_OK) {
        return err;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t h;
    err = nvs_open(CRED_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        xSemaphoreGive(s_lock);
        return err;
    }
    size_t len = cap;
    err = nvs_get_str(h, name, out, &len);
    nvs_close(h);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t credential_store_set(const char *name, const char *value)
{
    if (!credential_name_valid(name) || value == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(value) >= CRED_MAX_VALUE) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = credential_store_init();
    if (err != ESP_OK) {
        return err;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t h;
    err = nvs_open(CRED_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        xSemaphoreGive(s_lock);
        return err;
    }
    err = nvs_set_str(h, name, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        cached_value_invalidate(name); /* 清除旧 secret；下次读取时重新加载缓存 */
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t credential_store_erase(const char *name)
{
    if (!credential_name_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = credential_store_init();
    if (err != ESP_OK) {
        return err;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t h;
    err = nvs_open(CRED_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        xSemaphoreGive(s_lock);
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
        cached_value_invalidate(name);
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t credential_store_copy_runtime(const char *name, char *out, size_t cap)
{
    if (!credential_name_valid(name) || out == nullptr || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = credential_store_init();
    if (err != ESP_OK) {
        return err;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_loaded) {
        cache_load_all();
    }
    const char *value = nullptr;
    if (strcmp(name, NVS_KEY_WIFI_SSID) == 0) {
        value = s_wifi_ssid;
    } else if (strcmp(name, NVS_KEY_WIFI_PASS) == 0) {
        value = s_wifi_pass;
    } else if (strcmp(name, NVS_KEY_LLM_KEY) == 0) {
        value = s_llm_key;
    }
    if (value == nullptr) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_ARG;
    }
    int written = snprintf(out, cap, "%s", value);
    xSemaphoreGive(s_lock);
    if (written < 0 || (size_t)written >= cap) {
        cache_zero(out, cap);
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}
