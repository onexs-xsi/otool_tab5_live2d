#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LLM worker snapshot (filled by llm_app_get_status).
 */
typedef struct {
    int round;              /**< Last/current round index */
    bool busy;              /**< A request is in flight */
    size_t reply_len;       /**< Bytes accumulated in the reply buffer */
    char error[128];        /**< Last error message ("" if none) */
} llm_app_status_t;

/**
 * @brief Start the LLM worker (blocking requests on its own task).
 *
 * Does NOT create any UI; the UI is managed by ui_app_start(). The worker
 * disables itself cleanly when the effective API key is empty; otherwise it
 * waits for Wi-Fi (wifi_app) before the first request and then only acts on
 * triggers (tap / llm-ask console command).
 */
void llm_app_start(void);

/**
 * @brief Trigger a new LLM round immediately (tap / console entry).
 */
void llm_app_ask_now(void);

/**
 * @brief Ask a custom question (console entry: "llm-ask <text>").
 *
 * The text is queued for the worker; the next trigger uses it instead of the
 * default rotating question. A new ask_text() replaces any pending one.
 */
void llm_app_ask_text(const char *text);

/**
 * @brief Cancel the in-flight LLM request (console entry). Idempotent.
 */
void llm_app_cancel_now(void);

/**
 * @brief Fill a status snapshot (thread-safe).
 */
void llm_app_get_status(llm_app_status_t *out);

/**
 * @brief Copy the accumulated reply text out of the worker's buffer.
 *
 * @return Number of bytes copied (excluding NUL).
 */
size_t llm_app_reply_read(char *buf, size_t cap);

/**
 * @brief Copy the transient hint text (console feedback) out.
 */
void llm_app_hint_read(char *buf, size_t cap);

/**
 * @brief Set a transient status hint (console feedback).
 */
void llm_app_set_hint(const char *text);

#ifdef __cplusplus
}
#endif
