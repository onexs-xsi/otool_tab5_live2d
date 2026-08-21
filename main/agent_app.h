#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the agent app: tool registry + agent worker (own task).
 *
 * Registers the device tool `get_device_status` (read-only) and waits for
 * triggers from the console (`agent <text>`) / future UI. Runs the agent
 * loop (Responses remote chain) with the NVS credential.
 */
void agent_app_start(void);

/**
 * @brief Run one agent session with the given user text (replaces any active run).
 */
void agent_app_ask(const char *text);

/**
 * @brief Cancel the active agent run.
 */
void agent_app_cancel(void);

/**
 * @brief Fill a short status string (round/busy/reply length/error).
 */
void agent_app_status(char *buf, size_t cap);

/**
 * @brief Copy the accumulated final reply out.
 * @return bytes copied.
 */
size_t agent_app_reply_read(char *buf, size_t cap);

/**
 * @brief Current protocol name ("responses" or "chat").
 */
const char *agent_proto_name(void);

#ifdef __cplusplus
}
#endif
