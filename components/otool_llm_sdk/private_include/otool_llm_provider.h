/*
 * SPDX-FileCopyrightText: 2026 otool project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef OTOOL_LLM_PROVIDER_H
#define OTOOL_LLM_PROVIDER_H

#include "otool_llm_sdk.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Provider capability bits.
 */
#define OTOOL_LLM_PROVIDER_CAP_RESPONSES                (1u << 0) /**< Supports POST /responses + SSE */
#define OTOOL_LLM_PROVIDER_CAP_CHAT                     (1u << 1) /**< Supports POST /chat/completions + SSE */
#define OTOOL_LLM_PROVIDER_CAP_CHAT_STREAM_OPTIONS      (1u << 2) /**< Supports stream_options.include_usage */
#define OTOOL_LLM_PROVIDER_CAP_CHAT_MAX_COMPLETION_TOKENS (1u << 3) /**< Chat max-token field is max_completion_tokens (else max_tokens) */

/**
 * @brief Provider preset. Only provides defaults and provider-specific hooks;
 *        it knows nothing about event mapping (that is the protocol adapter's job).
 */
typedef struct {
    const char *name;
    const char *default_base_url;          /**< NULL = caller must supply base_url (custom) */
    const char *default_responses_path;
    const char *default_chat_path;
    otool_llm_protocol_t default_protocol;
    uint32_t capabilities;
    /**< Header name carrying the request/log id (case-insensitive); NULL = none */
    const char *request_id_header_name;
    /**< Build the Authorization header value from the api key (e.g. "Bearer <key>"). */
    esp_err_t (*build_auth_header)(const char *api_key, char *out, size_t out_size);
    /**< Extract provider error fields from a bounded non-2xx body. All out params optional (NULL = skip). */
    esp_err_t (*parse_provider_error)(const char *body, size_t len,
                                      char *out_message, size_t message_size,
                                      char *out_code, size_t code_size,
                                      char *out_request_id, size_t request_id_size);
} otool_llm_provider_preset_t;

/**
 * @brief Look up the preset for a provider id.
 */
const otool_llm_provider_preset_t *otool_llm_provider_get(otool_llm_provider_t id);

#ifdef __cplusplus
}
#endif

#endif /* OTOOL_LLM_PROVIDER_H */
