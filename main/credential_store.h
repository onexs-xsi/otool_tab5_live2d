#pragma once

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Effective application credential store.
 *
 * Local sdkconfig values have precedence. NVS is retained only as an optional
 * fallback when the corresponding sdkconfig value is empty. Repository
 * defaults remain empty and must never contain real credentials.
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

/**
 * @brief Copy one cached runtime credential while holding the store mutex.
 *
 * Names are the same as credential_store_get(). A non-empty sdkconfig value
 * wins over NVS; NVS is read only when that value is empty.
 */
esp_err_t credential_store_copy_runtime(const char *name, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
