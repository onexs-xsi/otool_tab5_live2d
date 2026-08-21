/*
 * SPDX-FileCopyrightText: 2026 otool project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef OTOOL_LLM_TRANSPORT_H
#define OTOOL_LLM_TRANSPORT_H

#include "otool_llm_sdk.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configuration for one HTTP SSE stream execution.
 *
 * The transport knows nothing about Responses/Chat event semantics: it performs
 * the request, validates status/content-type, feeds the SSE parser and forwards
 * parsed events to on_sse_event. Non-2xx bodies go to on_error_body.
 */
typedef struct {
    const char *url;              /**< Full URL (base + path) */
    const char *body;             /**< NUL-terminated JSON body */
    size_t body_len;
    const char *authorization;    /**< Full Authorization header value (provider-built) */
    int connect_timeout_ms;
    int read_timeout_ms;
    bool allow_insecure_http;     /**< Kconfig OTOOL_LLM_ALLOW_INSECURE_HTTP; local testing only */
    const char *request_id_header_name; /**< Header carrying request/log id (case-insensitive); NULL = none */
    char *request_id_out;         /**< Optional buffer filled from the header */
    size_t request_id_size;
    char *retry_after_out;        /**< Optional buffer filled from the Retry-After header */
    size_t retry_after_size;
    /**< One parsed SSE event at a time (event name + data). */
    esp_err_t (*on_sse_event)(void *arg, const char *event_name, const char *data, size_t data_len);
    void *sse_arg;
    /**< Bounded non-2xx error body (already capped; provider parser hook). */
    esp_err_t (*on_error_body)(void *arg, const char *body, size_t len);
    void *error_arg;
    /**< Cancel coordination: cancel_lock protects *cancel_requested and *active_http.
     *   Only the executing task sets/clears *active_http; any task may set the flag
     *   and call esp_http_client_cancel_request via the handle. */
    SemaphoreHandle_t cancel_lock;
    bool *cancel_requested;
    esp_http_client_handle_t *active_http;
} otool_llm_transport_config_t;

/**
 * @brief Execute one POST + SSE read. Blocks until the stream ends, an error
 *        occurs or a cancel is requested.
 *
 * @return
 *      - ESP_OK if the HTTP exchange completed and every SSE event was delivered
 *      - OTOOL_LLM_ERR_HTTP_STATUS after a non-2xx response (error body was parsed)
 *      - OTOOL_LLM_ERR_BAD_CONTENT_TYPE for 2xx without text/event-stream
 *      - OTOOL_LLM_ERR_EVENT_TOO_LARGE, OTOOL_LLM_ERR_PROTOCOL_EOF (SSE layer)
 *      - ESP_ERR_HTTP_* / other esp_err_t for transport, TLS and timeout failures
 */
esp_err_t otool_llm_transport_execute(const otool_llm_transport_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* OTOOL_LLM_TRANSPORT_H */
