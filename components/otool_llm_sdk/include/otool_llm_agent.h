/*
 * SPDX-FileCopyrightText: 2026 otool project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "otool_llm_sdk.h"
#include "otool_llm_text.h"
#include "otool_llm_tools.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief How conversation state is carried between turns.
 */
typedef enum {
    OTOOL_LLM_AGENT_STATE_REMOTE_RESPONSE_CHAIN = 0, /**< Responses: store=true + previous_response_id */
    OTOOL_LLM_AGENT_STATE_LOCAL_TRANSCRIPT,          /**< (future) local bounded transcript */
} otool_llm_agent_state_mode_t;

/**
 * @brief Policy decision for a tool call.
 */
typedef enum {
    OTOOL_LLM_TOOL_DECISION_ALLOW = 0,
    OTOOL_LLM_TOOL_DECISION_DENY,
} otool_llm_tool_decision_t;

/**
 * @brief Tool policy callback: decides whether a tool call may execute.
 */
typedef otool_llm_tool_decision_t (*otool_llm_tool_policy_cb_t)(
    const char *tool_name,
    const char *arguments_json,
    uint32_t tool_flags,
    void *user_ctx);

/**
 * @brief Agent configuration.
 */
typedef struct {
    size_t struct_size;
    otool_llm_client_handle_t client;              /**< Required */
    otool_llm_tool_registry_handle_t tools;        /**< Required; should be sealed before run */
    const char *model;                             /**< Required */
    const char *instructions;                      /**< Optional system instructions */
    otool_llm_agent_state_mode_t state_mode;       /**< MVP: REMOTE_RESPONSE_CHAIN */
    uint32_t max_turns;                            /**< Default 6 when 0 */
    uint32_t max_tool_calls;                       /**< Default 8 when 0 */
    uint32_t run_timeout_ms;                       /**< Default 120000 when 0 */
    bool parallel_tool_calls;                      /**< MVP: false */
    otool_llm_tool_policy_cb_t policy;             /**< NULL = side-effecting tools denied */
    void *policy_ctx;
} otool_llm_agent_config_t;

/**
 * @brief Agent run events. Exactly one terminal event per run:
 *        RUN_COMPLETED, RUN_LIMIT_REACHED, CANCELLED or ERROR.
 */
typedef enum {
    OTOOL_LLM_AGENT_EVENT_RUN_STARTED = 0,
    OTOOL_LLM_AGENT_EVENT_TURN_STARTED,
    OTOOL_LLM_AGENT_EVENT_TEXT_DELTA,
    OTOOL_LLM_AGENT_EVENT_TOOL_CALL_STARTED,
    OTOOL_LLM_AGENT_EVENT_TOOL_ARGUMENTS_DELTA,
    OTOOL_LLM_AGENT_EVENT_TOOL_CALL_READY,
    OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_STARTED,
    OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_FINISHED,
    OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_FAILED,
    OTOOL_LLM_AGENT_EVENT_USAGE,
    OTOOL_LLM_AGENT_EVENT_TURN_COMPLETED,
    OTOOL_LLM_AGENT_EVENT_RUN_COMPLETED,       /**< terminal */
    OTOOL_LLM_AGENT_EVENT_RUN_LIMIT_REACHED,   /**< terminal */
    OTOOL_LLM_AGENT_EVENT_CANCELLED,           /**< terminal */
    OTOOL_LLM_AGENT_EVENT_ERROR,               /**< terminal */
} otool_llm_agent_event_type_t;

typedef struct {
    otool_llm_agent_event_type_t type;
    uint32_t turn_index;
    uint32_t tool_index;    /**< Valid for tool_* events */
    const char *call_id;    /**< Valid for tool_* events; may be NULL */
    const char *name;       /**< Tool name; may be NULL */
    union {
        struct {
            const char *data;   /**< UTF-8 delta; valid until callback returns */
            size_t data_len;
        } text_delta;
        struct {
            const char *delta;
            size_t delta_len;
        } tool_arguments_delta;
        struct {
            const char *arguments;  /**< Complete arguments JSON */
            size_t arguments_len;
        } tool_call_ready;
        struct {
            const char *output;     /**< Tool output (stable JSON or text) */
            size_t output_len;
        } tool_execution_finished;
        struct {
            esp_err_t code;
            const char *message;
        } error;
        otool_llm_usage_t usage;
    } data;
} otool_llm_agent_event_t;

typedef otool_llm_event_action_t (*otool_llm_agent_event_cb_t)(const otool_llm_agent_event_t *event,
                                                               void *user_ctx);

typedef struct otool_llm_agent *otool_llm_agent_handle_t;

/**
 * @brief Create an agent. The client and registry must outlive the agent.
 */
esp_err_t otool_llm_agent_create(const otool_llm_agent_config_t *config,
                                 otool_llm_agent_handle_t *out_agent);

/**
 * @brief Run one agent session: user text -> (tool calls) -> final text.
 *
 * Blocks; runs callbacks in the calling (worker) task. One run per agent at a
 * time. Exactly one terminal agent event per run.
 *
 * @return ESP_OK on RUN_COMPLETED/RUN_LIMIT_REACHED/CANCELLED, the ERROR code
 *         otherwise.
 */
esp_err_t otool_llm_agent_run_stream(otool_llm_agent_handle_t agent,
                                     const char *user_text,
                                     otool_llm_agent_event_cb_t callback,
                                     void *user_ctx);

/**
 * @brief Cancel the running agent (any task). Idempotent.
 */
esp_err_t otool_llm_agent_cancel(otool_llm_agent_handle_t agent);

/**
 * @brief Reset the session (clears the response chain).
 */
void otool_llm_agent_reset_session(otool_llm_agent_handle_t agent);

/**
 * @brief Destroy the agent. Only valid when no run is active.
 */
void otool_llm_agent_destroy(otool_llm_agent_handle_t agent);

#ifdef __cplusplus
}
#endif
