/*
 * SPDX-FileCopyrightText: 2026 otool project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef OTOOL_LLM_TOOLS_H
#define OTOOL_LLM_TOOLS_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Tool side-effect classification (used by policy callbacks).
 */
typedef enum {
    OTOOL_LLM_TOOL_READ_ONLY        = 1u << 0, /**< No side effects */
    OTOOL_LLM_TOOL_IDEMPOTENT       = 1u << 1, /**< Safe to repeat */
    OTOOL_LLM_TOOL_SIDE_EFFECTING   = 1u << 2, /**< Mutates state / hardware */
    OTOOL_LLM_TOOL_NEEDS_APPROVAL   = 1u << 3, /**< Requires explicit user approval */
} otool_llm_tool_flags_t;

/**
 * @brief Cooperative cancellation/deadline handed to a tool callback.
 */
typedef struct {
    const volatile bool *cancel_requested; /**< Poll frequently; non-NULL */
    int64_t deadline_us;                   /**< Monotonic deadline (esp_timer), 0 = none */
} otool_llm_tool_exec_context_t;

/**
 * @brief Tool execution callback (synchronous, runs in the agent worker task).
 *
 * Must write a valid UTF-8 JSON object into output_json (bounded by
 * output_capacity) and check cancel/deadline periodically.
 */
typedef esp_err_t (*otool_llm_tool_execute_cb_t)(
    const char *arguments_json,
    char *output_json,
    size_t output_capacity,
    size_t *output_length,
    const otool_llm_tool_exec_context_t *exec_ctx,
    void *user_ctx);

/**
 * @brief One tool definition.
 *
 * @note Strings are only referenced during registration/request creation;
 *       the SDK deep-copies name/description/schema.
 */
typedef struct {
    size_t struct_size;                    /**< Must be sizeof(otool_llm_tool_definition_t) */
    const char *name;                      /**< Unique, safe chars only, non-empty */
    const char *description;               /**< Model-facing description */
    const char *parameters_json_schema;    /**< JSON Schema (object) as a string */
    bool strict;                           /**< Default true (provider strict mode) */
    uint32_t flags;                        /**< otool_llm_tool_flags_t bitmask */
    uint32_t timeout_ms;                   /**< Cooperative deadline for execute() */
    size_t max_output_bytes;               /**< Bound for output_json */
    otool_llm_tool_execute_cb_t execute;   /**< NULL = metadata-only (no executor) */
    void *user_ctx;                        /**< Passed to execute() */
} otool_llm_tool_definition_t;

/**
 * @brief One tool result to feed back into the model (function_call_output).
 *
 * @note Strings are only referenced during request_create(); the SDK deep-copies.
 */
typedef struct {
    const char *call_id;   /**< Must match the model's call_id */
    const char *output;    /**< JSON string, e.g. {"ok":true,...} */
} otool_llm_tool_output_t;

#ifdef __cplusplus
}
#endif

#endif /* OTOOL_LLM_TOOLS_H */
