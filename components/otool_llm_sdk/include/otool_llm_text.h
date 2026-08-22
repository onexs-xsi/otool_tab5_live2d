/*
 * SPDX-FileCopyrightText: 2026 otool project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef OTOOL_LLM_TEXT_H
#define OTOOL_LLM_TEXT_H

#include "otool_llm_sdk.h"
#include "otool_llm_tools.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Message roles.
 */
typedef enum {
    OTOOL_LLM_ROLE_DEVELOPER = 0,
    OTOOL_LLM_ROLE_SYSTEM,
    OTOOL_LLM_ROLE_USER,
    OTOOL_LLM_ROLE_ASSISTANT,
    OTOOL_LLM_ROLE_TOOL,   /**< Tool result message (Chat protocol); tool_call_id must be set */
} otool_llm_role_t;

/**
 * @brief One tool call inside an assistant message (Chat protocol, WP5).
 */
typedef struct {
    const char *id;        /**< tool call id (matches the model's call_id) */
    const char *name;
    const char *arguments; /**< Complete arguments JSON */
} otool_llm_tool_call_msg_t;

/**
 * @brief One text message. Strings are only referenced during request_create().
 */
typedef struct {
    otool_llm_role_t role;
    const char *text;
    /* Chat tool calling (WP5): */
    const otool_llm_tool_call_msg_t *tool_calls; /**< Assistant message tool calls; NULL = plain */
    size_t tool_call_count;
    const char *tool_call_id;  /**< Required when role == OTOOL_LLM_ROLE_TOOL */
} otool_llm_text_message_t;

/**
 * @brief Text generation request.
 *
 * @note Use OTOOL_LLM_TEXT_REQUEST_DEFAULT for `struct_size`.
 *       request_create() deep-copies model/instructions/messages/previous_response_id;
 *       the caller may free its strings right after a successful create.
 */
typedef struct {
    size_t struct_size;                 /**< Must be sizeof(otool_llm_text_request_t) */
    const char *model;                  /**< Required, e.g. "doubao-seed-1-6-250615" */
    const char *instructions;           /**< Optional system-level instructions (Responses); NULL = none */
    const otool_llm_text_message_t *messages; /**< Optional input messages; NULL with message_count 0 = none */
    size_t message_count;
    const char *previous_response_id;   /**< Responses only; rejected for Chat protocol */
    int max_output_tokens;              /**< 0 = not set */
    float temperature;                  /**< Only sent when temperature_is_set is true */
    bool temperature_is_set;
    bool store;                         /**< Responses only; default false. Rejected for Chat protocol. */
    /* ---- function calling (Responses) ---- */
    const otool_llm_tool_definition_t *tools;   /**< Tool definitions; NULL/0 = none */
    size_t tool_count;
    const otool_llm_tool_output_t *tool_outputs; /**< function_call_output items appended to input; NULL/0 = none */
    size_t tool_output_count;
} otool_llm_text_request_t;

/**
 * @brief Stream event types. Exactly one terminal event per request:
 *        COMPLETED, INCOMPLETE, CANCELLED or ERROR.
 */
typedef enum {
    OTOOL_LLM_TEXT_EVENT_RESPONSE_STARTED = 0, /**< Response/request id and model known */
    OTOOL_LLM_TEXT_EVENT_TEXT_DELTA,           /**< UTF-8 increment; only valid until callback returns */
    OTOOL_LLM_TEXT_EVENT_TEXT_DONE,            /**< Text content finished; no full text is resent */
    OTOOL_LLM_TEXT_EVENT_USAGE,                /**< Token usage; -1 fields mean unavailable */
    OTOOL_LLM_TEXT_EVENT_COMPLETED,            /**< Normal termination (terminal) */
    OTOOL_LLM_TEXT_EVENT_INCOMPLETE,           /**< Ended due to token/content limits (terminal) */
    OTOOL_LLM_TEXT_EVENT_CANCELLED,            /**< Local cancel finished (terminal) */
    OTOOL_LLM_TEXT_EVENT_ERROR,                /**< Transport/TLS/HTTP/provider/JSON/protocol error (terminal) */
    /* ---- function calling (Responses) ---- */
    OTOOL_LLM_TEXT_EVENT_TOOL_CALL_STARTED,    /**< function_call item announced */
    OTOOL_LLM_TEXT_EVENT_TOOL_ARGUMENTS_DELTA, /**< Arguments JSON increment (may be empty) */
    OTOOL_LLM_TEXT_EVENT_TOOL_CALL_DONE,       /**< Arguments complete; execute the tool */
} otool_llm_text_event_type_t;

/**
 * @brief Token usage. -1 means the provider did not report the value.
 */
typedef struct {
    int64_t input_tokens;
    int64_t output_tokens;
    int64_t total_tokens;
} otool_llm_usage_t;

/**
 * @brief One stream event.
 *
 * All string spans (delta, message, response_id, model, request_id) are only valid
 * until the callback returns. The SDK never accumulates the full answer.
 */
typedef struct {
    otool_llm_text_event_type_t type;
    const char *response_id;   /**< May be NULL until RESPONSE_STARTED */
    const char *model;         /**< May be NULL until RESPONSE_STARTED */
    const char *request_id;    /**< Provider request/log id if known; may be NULL */
    union {
        struct {
            const char *data;  /**< UTF-8 delta, may be empty */
            size_t data_len;
        } text_delta;
        otool_llm_usage_t usage;
        struct {
            esp_err_t code;          /**< otool_llm_err_t or esp_err_t */
            const char *message;     /**< Human readable, sanitized */
            const char *provider_code; /**< Provider error code string; may be NULL */
        } error;
        struct {
            const char *reason;      /**< e.g. "max_output_tokens"; may be NULL */
        } incomplete;
        /* function calling */
        struct {
            uint32_t output_index;   /**< Item slot in the current turn */
            const char *item_id;     /**< May be NULL */
            const char *call_id;     /**< May be NULL (Ark delta events omit it) */
            const char *name;        /**< Tool name; may be NULL until resolved */
        } tool_call_started;
        struct {
            uint32_t output_index;
            const char *call_id;     /**< May be NULL (Ark delta events omit it) */
            const char *delta;       /**< Arguments JSON increment; may be empty */
            size_t delta_len;
        } tool_arguments_delta;
        struct {
            uint32_t output_index;
            const char *call_id;     /**< May be NULL */
            const char *name;        /**< Tool name; may be NULL */
            const char *arguments;   /**< Complete arguments JSON (valid until callback returns) */
            size_t arguments_len;
        } tool_call_done;
    } data;
} otool_llm_text_event_t;

/**
 * @brief Callback return action.
 */
typedef enum {
    OTOOL_LLM_EVENT_ACTION_CONTINUE = 0,
    OTOOL_LLM_EVENT_ACTION_CANCEL,   /**< Equivalent to a cross-task otool_llm_request_cancel() */
} otool_llm_event_action_t;

/**
 * @brief Stream event callback. Must not call LVGL directly; copy deltas and post
 *        them to a UI queue instead.
 */
typedef otool_llm_event_action_t (*otool_llm_text_event_cb_t)(const otool_llm_text_event_t *event,
                                                              void *user_ctx);

/**
 * @brief Create a request. Deep-copies model, instructions, messages and
 *        previous_response_id; the caller may free its strings afterwards.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG invalid arguments (missing model, bad struct_size)
 *      - OTOOL_LLM_ERR_UNSUPPORTED request uses a field the selected protocol does not support
 *      - OTOOL_LLM_ERR_REQUEST_TOO_LARGE configured message/string budget exceeded
 *      - OTOOL_LLM_ERR_TOOL_SCHEMA invalid or over-budget tool definition
 *      - ESP_ERR_NO_MEM out of memory
 */
esp_err_t otool_llm_request_create(otool_llm_client_handle_t client,
                                   const otool_llm_text_request_t *request,
                                   otool_llm_request_handle_t *out_request);

/**
 * @brief Execute the request and stream events to the callback.
 *
 * Blocks and runs the callback in the calling task. The application must call this
 * from a non-LVGL worker task. Exactly one terminal event is produced per request.
 *
 * @return
 *      - ESP_OK if the terminal event was COMPLETED, INCOMPLETE or CANCELLED
 *      - the error code reported by the terminal ERROR event otherwise
 *      - ESP_ERR_INVALID_STATE if this request or its client already has an in-flight request
 *      - ESP_ERR_INVALID_ARG invalid arguments
 */
esp_err_t otool_llm_request_execute_stream(otool_llm_request_handle_t request,
                                           otool_llm_text_event_cb_t callback,
                                           void *user_ctx);

/**
 * @brief Cancel an in-flight request. Idempotent. May be called from any task.
 *
 * @note Must not be called from inside the callback (use the CANCEL action instead).
 */
esp_err_t otool_llm_request_cancel(otool_llm_request_handle_t request);

/**
 * @brief Destroy a request. Only valid after execute_stream() has returned;
 *        otherwise the call is refused and logged.
 */
void otool_llm_request_destroy(otool_llm_request_handle_t request);

#ifdef __cplusplus
}
#endif

#endif /* OTOOL_LLM_TEXT_H */
