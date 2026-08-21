/*
 * SPDX-FileCopyrightText: 2026 otool project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LLM stream event type.
 */
typedef enum {
    OTOOL_LLM_EVENT_CONNECTED = 0,  /**< HTTP connection established */
    OTOOL_LLM_EVENT_DELTA,          /**< A streaming content delta is available */
    OTOOL_LLM_EVENT_DONE,           /**< Stream finished normally */
    OTOOL_LLM_EVENT_ERROR,          /**< An error occurred */
} otool_llm_event_type_t;

/**
 * @brief LLM stream event passed to the callback.
 *
 * @note `data` is only valid during the callback invocation.
 */
typedef struct {
    otool_llm_event_type_t type;
    const char *data;   /**< For DELTA: content text; for ERROR: error message */
    size_t data_len;    /**< Length of @p data */
} otool_llm_event_t;

/**
 * @brief Stream event callback.
 *
 * @param[in] event Event data
 * @param[in] user_ctx User context passed to otool_llm_chat_stream()
 */
typedef void (*otool_llm_event_cb_t)(const otool_llm_event_t *event, void *user_ctx);

/**
 * @brief Chat streaming request configuration.
 */
typedef struct {
    const char *api_key;        /**< Bearer API key, e.g. "ark-..." */
    const char *model;          /**< Model ID, e.g. "doubao-seed-1-6-250615" */
    const char *system_prompt;  /**< Optional system prompt, can be NULL */
    const char *user_message;   /**< User message content */
    int max_tokens;             /**< Optional max tokens, <=0 means use server default */
    float temperature;          /**< Optional temperature, <0 means use server default */
    int timeout_ms;             /**< Optional HTTP timeout, <=0 means use Kconfig default */
    otool_llm_event_cb_t on_event;  /**< Required event callback */
    void *user_ctx;             /**< Optional user context passed to callback */
} otool_llm_chat_request_t;

/**
 * @brief Send a streaming chat completion request.
 *
 * This function blocks until the HTTP stream finishes or an error occurs.
 * The caller should normally run it in a dedicated task if the UI must stay responsive.
 *
 * @param[in] req Request configuration
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if req is NULL or required fields are missing
 *      - Other esp_http_client error codes on transport failure
 */
esp_err_t otool_llm_chat_stream(const otool_llm_chat_request_t *req);

#ifdef __cplusplus
}
#endif
