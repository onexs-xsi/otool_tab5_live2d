#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AGENT_APP_PHASE_BOOTING = 0,
    AGENT_APP_PHASE_CONNECTING,
    AGENT_APP_PHASE_READY,
    AGENT_APP_PHASE_LISTENING, /* Reserved for the future audio pipeline. */
    AGENT_APP_PHASE_THINKING,
    AGENT_APP_PHASE_TOOL,
    AGENT_APP_PHASE_RESPONDING,
    AGENT_APP_PHASE_COMPLETED,
    AGENT_APP_PHASE_CANCELLED,
    AGENT_APP_PHASE_DISABLED,
    AGENT_APP_PHASE_ERROR,
} agent_app_phase_t;

/**
 * @brief Start the agent app: tool registry + agent worker (own task).
 *
 * Registers `get_device_status` (read-only) and the policy-gated reversible
 * `set_ui_status` tool, then waits for console/UI triggers. Runs Responses
 * remote chaining or a Chat local transcript with the effective credential
 * (local sdkconfig first, optional NVS fallback).
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
 * @brief Cancel any active run and reset the conversation before the next run.
 */
void agent_app_reset_session(void);

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

/**
 * @brief Current high-level phase for UI presentation.
 */
agent_app_phase_t agent_app_phase(void);

#ifdef __cplusplus
}
#endif
