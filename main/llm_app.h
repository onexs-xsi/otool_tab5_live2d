#pragma once

#include <stddef.h>

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

/**
 * @brief Trigger a new LLM round immediately (console/touch entry).
 */
void llm_app_ask_now(void);

/**
 * @brief Cancel the in-flight LLM request (console entry). Idempotent.
 */
void llm_app_cancel_now(void);

/**
 * @brief Fill a short human-readable status string.
 */
void llm_app_status_str(char *buf, size_t size);

/**
 * @brief Set a transient status hint (console feedback).
 */
void llm_app_set_hint(const char *text);

#ifdef __cplusplus
}
#endif
