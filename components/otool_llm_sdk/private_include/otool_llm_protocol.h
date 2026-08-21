/*
 * SPDX-FileCopyrightText: 2026 otool project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef OTOOL_LLM_PROTOCOL_H
#define OTOOL_LLM_PROTOCOL_H

#include "otool_llm_provider.h"
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
 * @brief Internal deep-copied message (owned strings). Same shape as the public
 *        otool_llm_text_message_t but with mutable storage.
 */
typedef struct {
    otool_llm_role_t role;
    char *text;
} otool_llm_request_message_t;

/**
 * @brief Immutable view of the request data given to protocol adapters.
 *        Valid for the duration of one execute_stream() call.
 */
typedef struct {
    const char *model;
    const char *instructions;
    const otool_llm_request_message_t *messages;
    size_t message_count;
    const char *previous_response_id;
    int max_output_tokens;
    float temperature;
    bool temperature_is_set;
    bool store;
    const otool_llm_tool_definition_t *tools;   /**< NULL/0 = none */
    size_t tool_count;
    const otool_llm_tool_output_t *tool_outputs; /**< NULL/0 = none */
    size_t tool_output_count;
} otool_llm_request_view_t;

/**
 * @brief One streaming function call being accumulated (per output_index).
 */
typedef struct {
    bool active;            /**< Slot in use */
    bool arguments_done;    /**< Complete arguments received (done event or item done) */
    uint32_t output_index;
    char item_id[64];
    char call_id[64];
    char name[64];
    char arguments[4096];   /**< Accumulated arguments JSON (bounded) */
    size_t arguments_len;
} otool_llm_pending_tool_call_t;

#ifndef CONFIG_OTOOL_LLM_MAX_PENDING_TOOL_CALLS
#define CONFIG_OTOOL_LLM_MAX_PENDING_TOOL_CALLS 2
#endif

/**
 * @brief Responses adapter private state.
 */
typedef struct {
    bool started;            /**< response.created seen */
    bool text_done;          /**< response.output_text.done seen */
    otool_llm_pending_tool_call_t tool_calls[CONFIG_OTOOL_LLM_MAX_PENDING_TOOL_CALLS];
} otool_llm_responses_state_t;

/**
 * @brief Chat Completions adapter private state.
 */
typedef struct {
    bool saw_done;           /**< data: [DONE] seen */
    bool saw_usage;          /**< usage chunk seen */
    char finish_reason[32];  /**< last non-empty finish_reason */
} otool_llm_chat_state_t;

/**
 * @brief Execution context shared between the executor (request.c) and protocol
 *        adapters. String fields are owned by the executor and only valid during
 *        one execute_stream() call.
 */
typedef struct otool_llm_exec_ctx {
    const otool_llm_request_view_t *request;  /**< Request snapshot */
    const otool_llm_provider_preset_t *provider; /**< Resolved provider preset */
    const struct otool_llm_protocol_ops *protocol; /**< Resolved protocol adapter */
    void *request_owner;                      /**< Back-pointer used by the emitter for cancel */
    otool_llm_text_event_cb_t callback;       /**< Application callback */
    void *user_ctx;
    /**< Event emitter provided by the executor; adapters must use it. */
    esp_err_t (*emit)(struct otool_llm_exec_ctx *ctx, const otool_llm_text_event_t *evt);
    bool cancel_requested;                    /**< Callback returned CANCEL or cancel() was called */
    bool terminal_sent;                       /**< A terminal event was emitted */
    /**< Telemetry (filled by adapters). */
    char response_id[128];
    char model[64];
    char request_id[128];
    char retry_after[32];   /**< Retry-After header value from a non-2xx response */
    otool_llm_usage_t usage;
    bool usage_set;
    /**< Captured terminal error (filled by adapters before emitting ERROR). */
    esp_err_t error_code;
    char error_message[256];
    char provider_error_code[64];
    /**< Adapter private state. */
    union {
        otool_llm_responses_state_t responses;
        otool_llm_chat_state_t chat;
    } proto;
} otool_llm_exec_ctx_t;

/**
 * @brief Protocol adapter vtable. An adapter only builds request JSON and maps
 *        SSE events; it never touches esp_http_client.
 */
typedef struct otool_llm_protocol_ops {
    otool_llm_protocol_t id;
    const char *name;
    /**< Serialize the request into a bounded buffer. out_len = bytes written (no NUL). */
    esp_err_t (*build_request)(const otool_llm_request_view_t *req,
                               const otool_llm_provider_preset_t *provider,
                               char *out, size_t out_size, size_t *out_len);
    /**< Handle one parsed SSE event (event name + data, data_len bytes). */
    esp_err_t (*on_sse_event)(otool_llm_exec_ctx_t *ctx,
                              const char *event_name,
                              const char *data, size_t data_len);
    /**< Called when the HTTP stream ends without a terminal event; must emit the
     *   missing terminal event (normally ERROR/PROTOCOL_EOF). */
    esp_err_t (*on_eof)(otool_llm_exec_ctx_t *ctx);
} otool_llm_protocol_ops_t;

/**
 * @brief Resolve a protocol for a provider (AUTO -> provider default).
 *
 * @return ESP_OK, ESP_ERR_INVALID_ARG for bad ids / AUTO without a provider default,
 *         ESP_ERR_UNSUPPORTED if the provider lacks the capability.
 */
esp_err_t otool_llm_protocol_resolve(otool_llm_provider_t provider_id,
                                     otool_llm_protocol_t protocol_id,
                                     const otool_llm_provider_preset_t **out_provider,
                                     const otool_llm_protocol_ops_t **out_ops);

/**
 * @brief Adapter helper: set a captured error and emit a terminal ERROR event.
 */
esp_err_t otool_llm_exec_report_error(otool_llm_exec_ctx_t *ctx, esp_err_t code,
                                      const char *message, const char *provider_code);

/**< Responses SSE adapter (OpenAI Responses API / Ark /responses). */
extern const otool_llm_protocol_ops_t otool_llm_protocol_responses;
/**< Chat Completions SSE adapter (legacy / fallback). */
extern const otool_llm_protocol_ops_t otool_llm_protocol_chat;

#ifdef __cplusplus
}
#endif

#endif /* OTOOL_LLM_PROTOCOL_H */
