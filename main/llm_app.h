#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the LLM demo app: ESP-Hosted (C6) Wi-Fi + Doubao streaming chat + LVGL UI.
 *
 * @param tab5_comp Pointer to the initialized otool_tab5_component (used to
 *                  power the C6 module via WLAN_PWR_EN).
 */
void llm_app_start(void *tab5_comp);

#ifdef __cplusplus
}
#endif
