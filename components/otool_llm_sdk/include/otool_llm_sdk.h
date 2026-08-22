/*
 * SPDX-FileCopyrightText: 2026 otool project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef OTOOL_LLM_SDK_H
#define OTOOL_LLM_SDK_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OTOOL_LLM_SDK_VERSION_MAJOR 0
#define OTOOL_LLM_SDK_VERSION_MINOR 3
#define OTOOL_LLM_SDK_VERSION_PATCH 0

/**
 * @brief Component error base (0x1D000 range is not used by other IDF components).
 */
#define OTOOL_LLM_ERR_BASE (0x1D000)

/**
 * @brief otool_llm_sdk specific error codes.
 *
 * All values are esp_err_t compatible.
 */
typedef enum {
    OTOOL_LLM_ERR_PROTOCOL = OTOOL_LLM_ERR_BASE,   /**< Protocol violation or malformed provider event */
    OTOOL_LLM_ERR_PROTOCOL_EOF,                    /**< Stream ended before a terminal event */
    OTOOL_LLM_ERR_JSON,                            /**< JSON parse or type error */
    OTOOL_LLM_ERR_EVENT_TOO_LARGE,                 /**< SSE event exceeded OTOOL_LLM_MAX_SSE_EVENT_BYTES */
    OTOOL_LLM_ERR_REQUEST_TOO_LARGE,               /**< Serialized request exceeded OTOOL_LLM_MAX_REQUEST_BYTES */
    OTOOL_LLM_ERR_ERROR_BODY_TOO_LARGE,            /**< Non-2xx error body exceeded OTOOL_LLM_MAX_ERROR_BODY_BYTES */
    OTOOL_LLM_ERR_PROVIDER,                        /**< Provider reported an error (details in ERROR event) */
    OTOOL_LLM_ERR_HTTP_STATUS,                     /**< Non-2xx HTTP status */
    OTOOL_LLM_ERR_BAD_CONTENT_TYPE,                /**< 2xx response but Content-Type is not text/event-stream */
    OTOOL_LLM_ERR_UNSUPPORTED,                     /**< Request field not supported by selected protocol/provider */
    OTOOL_LLM_ERR_BUSY,                            /**< Client already has an in-flight request */
    OTOOL_LLM_ERR_TERMINATED,                      /**< Request has already produced a terminal event */
    OTOOL_LLM_ERR_TOOL_NOT_FOUND,                  /**< Tool name not registered */
    OTOOL_LLM_ERR_TOOL_SCHEMA,                     /**< Tool schema invalid / unsupported keywords */
    OTOOL_LLM_ERR_TOOL_ARGUMENTS,                  /**< Tool arguments invalid or over budget */
    OTOOL_LLM_ERR_TOOL_OUTPUT_TOO_LARGE,           /**< Tool output exceeds budget */
    OTOOL_LLM_ERR_TOOL_FAILED,                     /**< Tool callback failed */
    OTOOL_LLM_ERR_TOOL_DENIED,                     /**< Tool rejected by policy */
    OTOOL_LLM_ERR_AGENT_LIMIT,                     /**< Agent run limit reached */
    OTOOL_LLM_ERR_CONTEXT_FULL,                    /**< Local transcript budget exhausted */
} otool_llm_err_t;

/**
 * @brief Human readable name for otool_llm_sdk error codes.
 */
const char *otool_llm_err_to_name(esp_err_t err);

typedef struct otool_llm_client *otool_llm_client_handle_t;
typedef struct otool_llm_request *otool_llm_request_handle_t;

/**
 * @brief Supported LLM providers (presets).
 */
typedef enum {
    OTOOL_LLM_PROVIDER_OPENAI = 0,
    OTOOL_LLM_PROVIDER_VOLCENGINE_ARK,
    OTOOL_LLM_PROVIDER_CUSTOM,
} otool_llm_provider_t;

/**
 * @brief Text protocols. Responses API is the primary protocol.
 */
typedef enum {
    OTOOL_LLM_PROTOCOL_AUTO = 0,             /**< Resolve to provider default (never sends probe requests) */
    OTOOL_LLM_PROTOCOL_RESPONSES_SSE,        /**< POST /responses with SSE */
    OTOOL_LLM_PROTOCOL_CHAT_COMPLETIONS_SSE, /**< POST /chat/completions with SSE */
} otool_llm_protocol_t;

/**
 * @brief Client configuration.
 *
 * @note Use OTOOL_LLM_CLIENT_CONFIG_DEFAULT for `struct_size` and defaults,
 *       or initialize `struct_size` to sizeof(otool_llm_client_config_t).
 *       New fields are only ever appended at the tail.
 */
typedef struct {
    size_t struct_size;              /**< Must be sizeof(otool_llm_client_config_t) */
    otool_llm_provider_t provider;   /**< Provider preset */
    otool_llm_protocol_t protocol;   /**< Protocol; AUTO is invalid for OTOOL_LLM_PROVIDER_CUSTOM */
    const char *base_url;            /**< Full base URL including API prefix (e.g. "https://api.openai.com/v1"); NULL = provider default */
    const char *responses_path;      /**< Path appended to base_url for Responses; NULL = default ("/responses") */
    const char *chat_path;           /**< Path appended to base_url for Chat Completions; NULL = default ("/chat/completions") */
    const char *api_key;             /**< Bearer API key; deep-copied at create, must NOT come from a compile-time macro */
    int connect_timeout_ms;          /**< Connect timeout; <= 0 = Kconfig default */
    int read_timeout_ms;             /**< Stream read timeout; <= 0 = Kconfig default */
} otool_llm_client_config_t;

/**
 * @brief Create a client. Deep-copies all strings; caller may free them afterwards.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG invalid arguments (provider/protocol combination, missing api_key)
 *      - ESP_ERR_INVALID_SIZE URL/path/key exceeds its configured boundary
 *      - ESP_ERR_INVALID_VERSION struct_size mismatch
 *      - ESP_ERR_NO_MEM out of memory
 */
esp_err_t otool_llm_client_create(const otool_llm_client_config_t *config,
                                  otool_llm_client_handle_t *out_client);

/**
 * @brief Destroy a client.
 *
 * @note Refuses (and logs) if a request is still in flight.
 */
void otool_llm_client_destroy(otool_llm_client_handle_t client);

#ifdef __cplusplus
}
#endif

#endif /* OTOOL_LLM_SDK_H */
