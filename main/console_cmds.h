#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the USB-Serial-JTAG console REPL and register commands.
 *        Blocks in the REPL task; call once after llm_app_start().
 */
void console_start(void);

#ifdef __cplusplus
}
#endif
