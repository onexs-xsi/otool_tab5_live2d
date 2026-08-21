#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the LLM app: Doubao streaming chat + LVGL UI.
 *
 * Wi-Fi is managed separately by wifi_app_start(); the LLM worker waits for
 * the connection before the first request.
 */
void llm_app_start(void);

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
