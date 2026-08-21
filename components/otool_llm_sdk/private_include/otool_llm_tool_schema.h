/*
 * SPDX-FileCopyrightText: 2026 otool project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef OTOOL_LLM_TOOL_SCHEMA_H
#define OTOOL_LLM_TOOL_SCHEMA_H

#include "otool_llm_sdk.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Validate a parameters JSON Schema string against the supported subset.
 *        Called at registration time (plan §9: two-layer validation).
 */
esp_err_t otool_llm_tool_schema_validate(const char *schema_json, size_t schema_len);

/**
 * @brief Validate a model-provided arguments instance against the schema.
 */
esp_err_t otool_llm_tool_schema_check_arguments(const char *schema_json, size_t schema_len,
                                                const char *arguments_json, size_t args_len);

#ifdef __cplusplus
}
#endif

#endif /* OTOOL_LLM_TOOL_SCHEMA_H */
