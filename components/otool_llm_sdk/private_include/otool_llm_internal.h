/*
 * SPDX-FileCopyrightText: 2026 otool project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef OTOOL_LLM_INTERNAL_H
#define OTOOL_LLM_INTERNAL_H

#include "otool_llm_protocol.h"
#include "otool_llm_sdk.h"
#include "otool_llm_text.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct otool_llm_client {
    otool_llm_provider_t provider_id;
    otool_llm_protocol_t protocol_id;
    const otool_llm_provider_preset_t *provider;  /**< Resolved preset */
    const otool_llm_protocol_ops_t *protocol;  /**< Resolved adapter */
    char *base_url;        /**< Full base URL prefix (deep copy) */
    char *responses_path;  /**< May be NULL = provider default */
    char *chat_path;       /**< May be NULL = provider default */
    char *api_key;         /**< Deep copy; secure-zeroed on destroy */
    int connect_timeout_ms;
    int read_timeout_ms;
    SemaphoreHandle_t lock;
    otool_llm_request_handle_t active;  /**< In-flight request or NULL */
    uint32_t request_count;             /**< 存活的 request handle 数（生命周期保护，P1） */
};

struct otool_llm_request {
    otool_llm_client_handle_t client;
    /* Deep-copied request data */
    char *model;
    char *instructions;
    otool_llm_request_message_t *messages;
    size_t message_count;
    char *previous_response_id;
    int max_output_tokens;
    float temperature;
    bool temperature_is_set;
    bool store;
    /* Execution state */
    SemaphoreHandle_t lock;
    bool executing;
    bool cancel_requested;
    esp_http_client_handle_t http;  /**< Active HTTP handle during execute (protected by lock) */
    otool_llm_exec_ctx_t exec;      /**< Execution context (valid during execute) */
};

/**
 * @brief Deep-copy a string (NULL in -> NULL out). Returns ESP_ERR_NO_MEM on failure.
 */
esp_err_t otool_llm_strdup(const char *s, char **out);

/**
 * @brief Zero memory in a way that is not optimized away.
 */
void otool_llm_secure_zero(void *ptr, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* OTOOL_LLM_INTERNAL_H */
