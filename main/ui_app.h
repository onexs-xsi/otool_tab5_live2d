#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Build the LLM status UI (status line, reply area, tap-to-ask) and
 *        start the periodic refresh timer. Reads LLM state through llm_app_*.
 */
void ui_app_start(void);

#ifdef __cplusplus
}
#endif
