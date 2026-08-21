#pragma once

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Runtime credential store (NVS-backed).
 *
 * WP0: 仓库与固件默认配置不包含真实凭证；Wi-Fi 密码与 LLM API Key 通过
 * console `cred set` 写入 NVS，应用启动时经此模块读取。
 *
 * Names: "wifi_ssid" / "wifi_pass" / "llm_key".
 */

/**
 * @brief Open the NVS namespace used by the store. Call once at startup.
 */
esp_err_t credential_store_init(void);

/**
 * @brief Get a stored credential. Returns ESP_OK with value copied, or
 *        ESP_ERR_NOT_FOUND if unset.
 */
esp_err_t credential_store_get(const char *name, char *out, size_t cap);

/**
 * @brief Set/overwrite a credential (persisted to NVS).
 */
esp_err_t credential_store_set(const char *name, const char *value);

/**
 * @brief Erase a credential.
 */
esp_err_t credential_store_erase(const char *name);

/* ---- helpers returning cached copies (safe to call anytime after init) ---- */

/**< Wi-Fi SSID: NVS first, Kconfig default as fallback (not secret). */
const char *credential_wifi_ssid(void);
/**< Wi-Fi password: NVS only; "" when unset. */
const char *credential_wifi_password(void);
/**< LLM API key: NVS only; "" when unset. */
const char *credential_llm_key(void);

#ifdef __cplusplus
}
#endif
