#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start Wi-Fi via ESP-Hosted (C6 coprocessor).
 *
 * Powers the C6 module (WLAN_PWR_EN via IO expander), initializes the network
 * stack and the STA interface, and starts automatic connection to the SSID
 * configured in Kconfig (OTOOL_WIFI_SSID / OTOOL_WIFI_PASSWORD) with retries.
 *
 * @param tab5_comp Pointer to the initialized otool_tab5_component (used for
 *                  C6 power control). May be NULL.
 * @return ESP_OK on success (init done; connection happens asynchronously).
 */
esp_err_t wifi_app_start(void *tab5_comp);

/**
 * @brief Whether the STA currently has an IP (connected).
 */
bool wifi_app_is_connected(void);

/**
 * @brief Block until connected (IP acquired) or timeout.
 *
 * @param timeout_ms Timeout in ms; 0 = wait forever.
 * @return ESP_OK on connected, ESP_ERR_TIMEOUT on timeout.
 */
esp_err_t wifi_app_wait_connected(uint32_t timeout_ms);

/**
 * @brief Disconnect and reconnect the STA (console / manual trigger).
 */
esp_err_t wifi_app_reconnect(void);

#ifdef __cplusplus
}
#endif
