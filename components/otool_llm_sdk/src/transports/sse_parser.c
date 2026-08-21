/*
 * SPDX-FileCopyrightText: 2026 otool project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pure byte-level SSE state machine (WHATWG Server-Sent Events processing model).
 * No dependency on esp_http_client or any provider/protocol code.
 */

#include "otool_llm_sse_parser.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define OTOOL_SSE_EVENT_NAME_CAP 64
#define OTOOL_SSE_ID_CAP 512
/* A single line is field name + value; give the name/prefix room beyond the
 * merged-data cap (e.g. "event:" + 63 chars or "data:" + max bytes). */
#define OTOOL_SSE_LINE_HEADROOM 128

struct otool_llm_sse_parser {
    size_t max_event_bytes;
    /* current line */
    char *line;
    size_t line_len;
    /* event accumulation */
    char *data;
    size_t data_len;
    char *event_name;
    char *cur_id;   /* event id of the event being accumulated */
    char *last_id;  /* persisted last event id */
    bool last_id_set;
    bool pending_event; /* a dispatched event is sitting in the buffers: the next
                           feed()/finish() call clears them first, so the event's
                           spans stay valid between calls (transport reads them
                           inside the on_sse_event callback) */
};

otool_llm_sse_parser_t *otool_llm_sse_parser_create(size_t max_event_bytes)
{
    if (max_event_bytes == 0) {
        return NULL;
    }
    otool_llm_sse_parser_t *p = (otool_llm_sse_parser_t *)calloc(1, sizeof(*p));
    if (p == NULL) {
        return NULL;
    }
    /* One block: line + data + event name + current id + last id. */
    size_t total = max_event_bytes + OTOOL_SSE_LINE_HEADROOM + 1 /* line (incl. NUL) */
                 + max_event_bytes + 1          /* data (incl. NUL) */
                 + OTOOL_SSE_EVENT_NAME_CAP
                 + OTOOL_SSE_ID_CAP
                 + OTOOL_SSE_ID_CAP;
    char *block = (char *)calloc(1, total);
    if (block == NULL) {
        free(p);
        return NULL;
    }
    p->max_event_bytes = max_event_bytes;
    p->line = block;
    p->data = block + (max_event_bytes + OTOOL_SSE_LINE_HEADROOM + 1);
    p->event_name = p->data + (max_event_bytes + 1);
    p->cur_id = p->event_name + OTOOL_SSE_EVENT_NAME_CAP;
    p->last_id = p->cur_id + OTOOL_SSE_ID_CAP;
    return p;
}

void otool_llm_sse_parser_destroy(otool_llm_sse_parser_t *p)
{
    if (p == NULL) {
        return;
    }
    free(p->line); /* block start */
    free(p);
}

static otool_llm_sse_feed_result_t sse_dispatch(otool_llm_sse_parser_t *p,
                                                otool_llm_sse_event_t *out_event)
{
    /* Per spec: an event with an empty data buffer is not dispatched. */
    if (p->data_len == 0) {
        p->event_name[0] = '\0';
        p->cur_id[0] = '\0';
        return OTOOL_LLM_SSE_FEED_OK;
    }

    /* Persist the event id if non-empty. */
    if (p->cur_id[0] != '\0') {
        memcpy(p->last_id, p->cur_id, OTOOL_SSE_ID_CAP);
        p->last_id_set = true;
    }

    out_event->event = p->event_name[0] != '\0' ? p->event_name : "message";
    out_event->data = p->data;
    out_event->data_len = p->data_len;
    out_event->id = p->last_id_set ? p->last_id : NULL;

    /* Defer clearing until the next feed()/finish() call so the dispatched
     * event's spans stay valid between calls. */
    p->pending_event = true;
    return OTOOL_LLM_SSE_FEED_EVENT;
}

static void sse_clear_pending_event(otool_llm_sse_parser_t *p)
{
    if (!p->pending_event) {
        return;
    }
    p->data_len = 0;
    p->data[0] = '\0';
    p->event_name[0] = '\0';
    p->cur_id[0] = '\0';
    p->pending_event = false;
}

static otool_llm_sse_feed_result_t sse_process_line(otool_llm_sse_parser_t *p,
                                                    otool_llm_sse_event_t *out_event)
{
    const char *line = p->line;
    size_t line_len = p->line_len;

    /* Blank line: dispatch a complete event. */
    if (line_len == 0) {
        return sse_dispatch(p, out_event);
    }

    /* Comment lines start with ':'. */
    if (line[0] == ':') {
        return OTOOL_LLM_SSE_FEED_OK;
    }

    /* Split at the first ':'. */
    const char *colon = (const char *)memchr(line, ':', line_len);
    size_t field_len = colon != NULL ? (size_t)(colon - line) : line_len;
    const char *value = colon != NULL ? colon + 1 : line + line_len;
    size_t value_len = line_len - field_len - (colon != NULL ? 1 : 0);
    /* Strip at most one leading space. */
    if (value_len > 0 && value[0] == ' ') {
        value++;
        value_len--;
    }

    if (field_len == 4 && memcmp(line, "data", 4) == 0) {
        /* Append value; separate multiple data lines with '\n'. */
        size_t sep = p->data_len > 0 ? 1 : 0;
        if (p->data_len + sep + value_len > p->max_event_bytes) {
            return OTOOL_LLM_SSE_FEED_ERROR; /* hard cap, never truncate */
        }
        if (sep) {
            p->data[p->data_len++] = '\n';
        }
        if (value_len > 0) {
            memcpy(p->data + p->data_len, value, value_len);
            p->data_len += value_len;
        }
        p->data[p->data_len] = '\0';
        return OTOOL_LLM_SSE_FEED_OK;
    }

    if (field_len == 5 && memcmp(line, "event", 5) == 0) {
        if (value_len >= OTOOL_SSE_EVENT_NAME_CAP) {
            return OTOOL_LLM_SSE_FEED_ERROR;
        }
        memcpy(p->event_name, value, value_len);
        p->event_name[value_len] = '\0';
        return OTOOL_LLM_SSE_FEED_OK;
    }

    if (field_len == 2 && memcmp(line, "id", 2) == 0) {
        /* Per spec, an id containing NUL makes the whole line ignored. */
        if (memchr(value, '\0', value_len) != NULL) {
            return OTOOL_LLM_SSE_FEED_OK;
        }
        if (value_len >= OTOOL_SSE_ID_CAP) {
            return OTOOL_LLM_SSE_FEED_ERROR;
        }
        memcpy(p->cur_id, value, value_len);
        p->cur_id[value_len] = '\0';
        return OTOOL_LLM_SSE_FEED_OK;
    }

    /* "retry" and unknown fields are ignored. */
    return OTOOL_LLM_SSE_FEED_OK;
}

otool_llm_sse_feed_result_t otool_llm_sse_parser_feed(otool_llm_sse_parser_t *p,
                                                      const uint8_t *bytes, size_t len,
                                                      otool_llm_sse_event_t *out_event,
                                                      size_t *consumed)
{
    if (p == NULL || bytes == NULL || out_event == NULL || consumed == NULL) {
        return OTOOL_LLM_SSE_FEED_ERROR;
    }
    *consumed = 0;

    sse_clear_pending_event(p);

    bool last_was_cr = false;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = bytes[i];

        if (b == '\n' && last_was_cr) {
            /* CRLF: EOL already processed at the CR. */
            last_was_cr = false;
            *consumed = i + 1;
            continue;
        }

        if (b == '\n' || b == '\r') {
            otool_llm_sse_feed_result_t r = sse_process_line(p, out_event);
            p->line_len = 0;
            last_was_cr = (b == '\r');
            *consumed = i + 1;
            if (r != OTOOL_LLM_SSE_FEED_OK) {
                return r;
            }
            continue;
        }

        last_was_cr = false;
        if (p->line_len >= p->max_event_bytes + OTOOL_SSE_LINE_HEADROOM) {
            *consumed = i + 1;
            return OTOOL_LLM_SSE_FEED_ERROR; /* single line over the hard cap */
        }
        p->line[p->line_len++] = (char)b;
        *consumed = i + 1;
    }

    return OTOOL_LLM_SSE_FEED_OK;
}

esp_err_t otool_llm_sse_parser_finish(otool_llm_sse_parser_t *p)
{
    if (p == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    sse_clear_pending_event(p);
    if (p->line_len > 0 || p->data_len > 0 || p->event_name[0] != '\0') {
        /* Half event pending at EOF: report protocol EOF, never fabricate a completion. */
        return OTOOL_LLM_ERR_PROTOCOL_EOF;
    }
    return ESP_OK;
}
