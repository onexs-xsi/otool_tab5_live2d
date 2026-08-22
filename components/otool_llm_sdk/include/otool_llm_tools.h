/*
 * SPDX-FileCopyrightText: 2026 otool project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef OTOOL_LLM_TOOLS_H
#define OTOOL_LLM_TOOLS_H

#include "otool_llm_sdk.h"
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
 * On ESP_OK, must write exactly one UTF-8 JSON object, set output_length to
 * its byte length, and place NUL at output_json[*output_length]. Must stay
 * within output_capacity and poll cancel/deadline periodically.
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
    bool strict;                           /**< Requires all properties and additionalProperties:false */
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

/* ---------------- tool registry ---------------- */

typedef struct otool_llm_tool_registry *otool_llm_tool_registry_handle_t;

/**
 * @brief Create an empty tool registry.
 *
 * @param out_reg Receives the handle.
 * @return ESP_OK, ESP_ERR_NO_MEM.
 */
esp_err_t otool_llm_tool_registry_create(otool_llm_tool_registry_handle_t *out_reg);

/**
 * @brief Add a tool. Deep-copies name/description/schema and validates the
 *        schema up-front (rejected at registration, never at call time).
 *
 * Requirements: unique safe name (<= OTOOL_LLM_MAX_TOOL_NAME_BYTES), schema
 * and output byte budgets, valid parameters_json_schema of the supported JSON
 * Schema subset, and registry not sealed.
 *
 * @return ESP_OK, ESP_ERR_TOOL_SCHEMA, ESP_ERR_INVALID_ARG (duplicate/bad name),
 *         ESP_ERR_NO_MEM, ESP_ERR_INVALID_STATE (sealed).
 */
esp_err_t otool_llm_tool_registry_add(otool_llm_tool_registry_handle_t reg,
                                      const otool_llm_tool_definition_t *tool);

/**
 * @brief Seal the registry: no more add/remove. Afterwards lookups are lock-free.
 */
esp_err_t otool_llm_tool_registry_seal(otool_llm_tool_registry_handle_t reg);

/** @brief Whether seal() has made this registry immutable. */
bool otool_llm_tool_registry_is_sealed(otool_llm_tool_registry_handle_t reg);

/**
 * @brief Destroy the registry (only when no agent/session uses it; see plan §6.1).
 */
void otool_llm_tool_registry_destroy(otool_llm_tool_registry_handle_t reg);

/**
 * @brief Find a tool by name (safe before and after seal).
 * @return Pointer owned by the registry, or NULL.
 */
const otool_llm_tool_definition_t *otool_llm_tool_registry_find(
    otool_llm_tool_registry_handle_t reg, const char *name);

/**< Number of tools currently in the registry. */
size_t otool_llm_tool_registry_count(otool_llm_tool_registry_handle_t reg);

/**< Tool at index i (0 <= i < count). */
const otool_llm_tool_definition_t *otool_llm_tool_registry_at(
    otool_llm_tool_registry_handle_t reg, size_t index);

/**
 * @brief Validate a tool-call arguments instance against a registered schema.
 *
 * @param tool Tool whose schema is checked (registry-owned).
 * @param arguments_json Model-provided arguments (untrusted).
 * @param arguments_len Length of arguments_json.
 * @return ESP_OK, OTOOL_LLM_ERR_TOOL_ARGUMENTS (validation failed), ESP_ERR_INVALID_ARG.
 */
esp_err_t otool_llm_tool_arguments_validate(const otool_llm_tool_definition_t *tool,
                                            const char *arguments_json, size_t arguments_len);

#ifdef __cplusplus
}
#endif

#endif /* OTOOL_LLM_TOOLS_H */
