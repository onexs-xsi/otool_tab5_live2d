/*
 * SPDX-FileCopyrightText: 2026 otool project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef OTOOL_LLM_SSE_PARSER_H
#define OTOOL_LLM_SSE_PARSER_H

#include "otool_llm_sdk.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Result of feeding bytes to the SSE parser.
 */
typedef enum {
    OTOOL_LLM_SSE_FEED_OK = 0,   /**< Bytes consumed; no complete event yet */
    OTOOL_LLM_SSE_FEED_EVENT,    /**< One complete event; out_event is valid */
    OTOOL_LLM_SSE_FEED_ERROR,    /**< Hard error (event too large); parser is unusable afterwards */
} otool_llm_sse_feed_result_t;

/**
 * @brief One dispatched SSE event.
 *
 * All pointers reference buffers owned by the parser and stay valid until the
 * next feed()/finish()/destroy() call.
 */
typedef struct {
    const char *event;  /**< Event name; "message" when the event field was not set */
    const char *data;   /**< Merged data (multiple data: lines joined with '\n'); NUL-terminated, may be "" */
    size_t data_len;
    const char *id;     /**< Last event id (persisted across events); NULL if never set */
} otool_llm_sse_event_t;

typedef struct otool_llm_sse_parser otool_llm_sse_parser_t;

/**
 * @brief Create a parser with a hard cap on one merged event's data size.
 *
 * @param max_event_bytes Cap for a single merged SSE data payload and for any
 *                        single line. Exceeding it is a hard error, never a truncation.
 * @return parser or NULL on OOM
 */
otool_llm_sse_parser_t *otool_llm_sse_parser_create(size_t max_event_bytes);

void otool_llm_sse_parser_destroy(otool_llm_sse_parser_t *parser);

/**
 * @brief Feed raw bytes (no NUL required). Dispatches at most one event per call:
 *        on OTOOL_LLM_SSE_FEED_EVENT, *consumed = bytes processed including the
 *        terminating blank line; call again with the remainder to drain the chunk.
 *
 * Handles CRLF/LF/CR line endings, comment lines, event/data/id/retry fields,
 * multi-line data and UTF-8 bytes split at arbitrary positions.
 *
 * @param[out] consumed Bytes consumed by this call (valid for every result).
 */
otool_llm_sse_feed_result_t otool_llm_sse_parser_feed(otool_llm_sse_parser_t *parser,
                                                      const uint8_t *data, size_t len,
                                                      otool_llm_sse_event_t *out_event,
                                                      size_t *consumed);

/**
 * @brief Signal EOF. If a half event is pending (partial line or undelivered data),
 *        returns OTOOL_LLM_ERR_PROTOCOL_EOF without fabricating a completion.
 */
esp_err_t otool_llm_sse_parser_finish(otool_llm_sse_parser_t *parser);

#ifdef __cplusplus
}
#endif

#endif /* OTOOL_LLM_SSE_PARSER_H */
