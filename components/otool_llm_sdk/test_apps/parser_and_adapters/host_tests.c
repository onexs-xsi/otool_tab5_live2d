/*
 * SPDX-FileCopyrightText: 2026 otool project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-side unit tests for the SSE parser and protocol adapters.
 * Built with TinyCC (or any C99 compiler) against a host shim for esp_err_t.
 *
 * Build:
 *   tcc -I ../../components/otool_llm_sdk/private_include \
 *       -I third_party/cjson \
 *       -o host_tests.exe host_tests.c \
 *       ../../components/otool_llm_sdk/src/transports/sse_parser.c \
 *       ../../components/otool_llm_sdk/src/protocols/responses_sse.c \
 *       ../../components/otool_llm_sdk/src/protocols/chat_completions_sse.c \
 *       ../../components/otool_llm_sdk/src/providers/provider_ark.c \
 *       ../../components/otool_llm_sdk/src/providers/provider_openai.c \
 *       ../../components/otool_llm_sdk/src/providers/provider_custom.c \
 *       third_party/cjson/cJSON.c
 */

#include "otool_llm_protocol.h"
#include "otool_llm_provider.h"
#include "otool_llm_sse_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- tiny test harness ---- */

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        g_checks++;                                                             \
        if (!(cond)) {                                                          \
            g_failures++;                                                       \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                         \
            printf(__VA_ARGS__);                                                \
            printf("\n");                                                       \
        }                                                                       \
    } while (0)

#define CHECK_STR(a, b) CHECK(strcmp((a), (b)) == 0, "expected \"%s\", got \"%s\"", (b), (a))
#define CHECK_EQ(a, b) CHECK((a) == (b), "expected %lld, got %lld", (long long)(b), (long long)(a))

/* ---- esp_err.h shim (host) ---- */

int esp_err_to_name_stub = 0;
const char *esp_err_to_name(esp_err_t code)
{
    (void)code;
    return "ERR";
}

/* esp_log shim */
void otool_llm_host_log_printf(const char *tag, const char *fmt, ...)
{
    (void)tag;
    (void)fmt;
}

/* Host stub for the executor helper (request.c owns the real one). */
esp_err_t otool_llm_exec_report_error(otool_llm_exec_ctx_t *ctx, esp_err_t code,
                                      const char *message, const char *provider_code)
{
    ctx->error_code = code;
    if (message != NULL) {
        snprintf(ctx->error_message, sizeof(ctx->error_message), "%s", message);
    } else {
        ctx->error_message[0] = '\0';
    }
    if (provider_code != NULL) {
        snprintf(ctx->provider_error_code, sizeof(ctx->provider_error_code), "%s", provider_code);
    } else {
        ctx->provider_error_code[0] = '\0';
    }

    otool_llm_text_event_t evt = {
        .type = OTOOL_LLM_TEXT_EVENT_ERROR,
    };
    evt.data.error.code = code;
    evt.data.error.message = ctx->error_message;
    evt.data.error.provider_code = ctx->provider_error_code[0] != '\0' ? ctx->provider_error_code : NULL;
    ctx->emit(ctx, &evt);
    return code;
}

/* ---- SSE parser tests ---- */

typedef struct {
    char event[64];
    char data[2048];
    size_t data_len;
    char id[128];
    bool have_id;
} captured_event_t;

static int feed_all(otool_llm_sse_parser_t *p, const char *chunk, captured_event_t *events,
                    int max_events)
{
    int count = 0;
    size_t off = 0;
    size_t len = strlen(chunk);
    while (off < len && count < max_events) {
        otool_llm_sse_event_t evt;
        size_t consumed = 0;
        otool_llm_sse_feed_result_t r = otool_llm_sse_parser_feed(p, (const uint8_t *)chunk + off,
                                                                   len - off, &evt, &consumed);
        if (r == OTOOL_LLM_SSE_FEED_ERROR) {
            return -1;
        }
        off += consumed;
        if (r == OTOOL_LLM_SSE_FEED_EVENT) {
            /* copy immediately: parser spans are only valid until the next feed() */
            captured_event_t *dst = &events[count];
            snprintf(dst->event, sizeof(dst->event), "%s", evt.event);
            if (evt.data_len < sizeof(dst->data)) {
                memcpy(dst->data, evt.data, evt.data_len);
                dst->data[evt.data_len] = '\0';
            } else {
                snprintf(dst->data, sizeof(dst->data), "<overflow>");
            }
            dst->data_len = evt.data_len;
            dst->have_id = evt.id != NULL;
            if (evt.id != NULL) {
                snprintf(dst->id, sizeof(dst->id), "%s", evt.id);
            } else {
                dst->id[0] = '\0';
            }
            count++;
        }
        if (r == OTOOL_LLM_SSE_FEED_OK) {
            break;
        }
    }
    return count;
}

static void test_sse_basic(void)
{
    otool_llm_sse_parser_t *p = otool_llm_sse_parser_create(16384);
    CHECK(p != NULL, "parser create");
    captured_event_t evts[8];
    int n = feed_all(p, "data: hello\n\n", evts, 8);
    CHECK_EQ(n, 1);
    if (n == 1) {
        CHECK_STR(evts[0].event, "message");
        CHECK_STR(evts[0].data, "hello");
        CHECK_EQ(evts[0].data_len, 5);
        CHECK(!evts[0].have_id, "no id");
    }
    otool_llm_sse_parser_destroy(p);
}

static void test_sse_line_endings(void)
{
    const char *cases[] = {
        "data: a\n\n",
        "data: a\r\n\r\n",
        "data: a\r\r",
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        otool_llm_sse_parser_t *p = otool_llm_sse_parser_create(16384);
        captured_event_t evts[4];
        int n = feed_all(p, cases[i], evts, 4);
        CHECK_EQ(n, 1);
        if (n == 1) {
            CHECK_STR(evts[0].data, "a");
        }
        otool_llm_sse_parser_destroy(p);
    }
}

static void test_sse_multi_data(void)
{
    otool_llm_sse_parser_t *p = otool_llm_sse_parser_create(16384);
    captured_event_t evts[4];
    int n = feed_all(p, "data: line1\ndata: line2\ndata: line3\n\n", evts, 4);
    CHECK_EQ(n, 1);
    if (n == 1) {
        CHECK_STR(evts[0].data, "line1\nline2\nline3");
        CHECK_EQ(evts[0].data_len, 17);
    }
    otool_llm_sse_parser_destroy(p);
}

static void test_sse_comment_unknown_retry(void)
{
    otool_llm_sse_parser_t *p = otool_llm_sse_parser_create(16384);
    captured_event_t evts[4];
    int n = feed_all(p, ": comment\nretry: 1000\nunknown: x\nX-A: b\ndata: ok\n\n", evts, 4);
    CHECK_EQ(n, 1);
    if (n == 1) {
        CHECK_STR(evts[0].data, "ok");
    }
    otool_llm_sse_parser_destroy(p);
}

static void test_sse_event_name_and_id(void)
{
    otool_llm_sse_parser_t *p = otool_llm_sse_parser_create(16384);
    captured_event_t evts[4];
    int n = feed_all(p, "event: response.created\nid: 42\ndata: {\"a\":1}\n\n", evts, 4);
    CHECK_EQ(n, 1);
    if (n == 1) {
        CHECK_STR(evts[0].event, "response.created");
        CHECK(evts[0].have_id, "id present");
        CHECK_STR(evts[0].id, "42");
        CHECK_STR(evts[0].data, "{\"a\":1}");
    }
    /* last id persists into the next event */
    int n2 = feed_all(p, "data: second\n\n", evts + 1, 3);
    CHECK_EQ(n2, 1);
    CHECK(evts[1].have_id, "id persisted");
    CHECK_STR(evts[1].id, "42");
    otool_llm_sse_parser_destroy(p);
}

static void test_sse_empty_data_no_dispatch(void)
{
    otool_llm_sse_parser_t *p = otool_llm_sse_parser_create(16384);
    captured_event_t evts[4];
    int n = feed_all(p, "data:\n\n", evts, 4);
    CHECK_EQ(n, 0);
    /* event with no data: no dispatch either */
    int n2 = feed_all(p, "event: foo\ndata:\n\n", evts, 4);
    CHECK_EQ(n2, 0);
    otool_llm_sse_parser_destroy(p);
}

static void test_sse_multiple_events_one_chunk(void)
{
    otool_llm_sse_parser_t *p = otool_llm_sse_parser_create(16384);
    captured_event_t evts[8];
    int n = feed_all(p, "data: one\n\ndata: two\n\ndata: three\n\n", evts, 8);
    CHECK_EQ(n, 3);
    if (n == 3) {
        CHECK_STR(evts[0].data, "one");
        CHECK_STR(evts[1].data, "two");
        CHECK_STR(evts[2].data, "three");
    }
    otool_llm_sse_parser_destroy(p);
}

static void test_sse_value_leading_space(void)
{
    otool_llm_sse_parser_t *p = otool_llm_sse_parser_create(16384);
    captured_event_t evts[4];
    int n = feed_all(p, "data:  spaced\n\n", evts, 4);
    CHECK_EQ(n, 1);
    if (n == 1) {
        CHECK_STR(evts[0].data, " spaced"); /* only ONE leading space is stripped */
    }
    otool_llm_sse_parser_destroy(p);
}

static void test_sse_sharding(void)
{
    const char *stream = "event: response.output_text.delta\nid: 7\ndata: {\"delta\":\"你好👋\"}\n\n"
                         "data: second\n\n";
    size_t len = strlen(stream);
    /* Try every split position: feed(0..i) then feed(i..len); both must produce
       the same event sequence as feeding the whole stream at once. */
    for (size_t split = 0; split <= len; split++) {
        captured_event_t e1[8], e2[8];
        otool_llm_sse_parser_t *whole = otool_llm_sse_parser_create(16384);
        int n1 = feed_all(whole, stream, e1, 8);
        otool_llm_sse_parser_t *split_p = otool_llm_sse_parser_create(16384);
        int n2 = 0;
        if (split > 0) {
            char *part = (char *)malloc(split + 1);
            memcpy(part, stream, split);
            part[split] = '\0';
            n2 = feed_all(split_p, part, e2, 8);
            free(part);
        }
        n2 += feed_all(split_p, stream + split, e2 + n2, 8 - n2);
        CHECK_EQ(n1, n2);
        for (int i = 0; i < n1 && i < n2; i++) {
            CHECK_STR(e1[i].event, e2[i].event);
            CHECK_STR(e1[i].data, e2[i].data);
            CHECK_EQ(e1[i].data_len, e2[i].data_len);
            CHECK(e1[i].have_id == e2[i].have_id, "id null mismatch at split %d", (int)split);
            if (e1[i].have_id) {
                CHECK_STR(e1[i].id, e2[i].id);
            }
        }
        otool_llm_sse_parser_destroy(whole);
        otool_llm_sse_parser_destroy(split_p);
    }
}

static void test_sse_byte_sharding(void)
{
    /* feed one byte at a time (covers UTF-8 split inside the multibyte sequence) */
    const char *stream = "data: {\"delta\":\"你好👋世界\"}\n\ndata: x\n\n";
    otool_llm_sse_parser_t *p = otool_llm_sse_parser_create(16384);
    captured_event_t evts[8];
    size_t off = 0;
    size_t len = strlen(stream);
    int count = 0;
    while (off < len) {
        otool_llm_sse_event_t evt;
        size_t consumed = 0;
        otool_llm_sse_feed_result_t r = otool_llm_sse_parser_feed(p, (const uint8_t *)stream + off,
                                                                   1, &evt, &consumed);
        if (r == OTOOL_LLM_SSE_FEED_EVENT) {
            captured_event_t *dst = &evts[count];
            snprintf(dst->event, sizeof(dst->event), "%s", evt.event);
            memcpy(dst->data, evt.data, evt.data_len);
            dst->data[evt.data_len] = '\0';
            dst->data_len = evt.data_len;
            dst->have_id = evt.id != NULL;
            dst->id[0] = '\0';
            count++;
        }
        off += consumed;
        if (r == OTOOL_LLM_SSE_FEED_ERROR) {
            CHECK(0, "byte-shard feed error");
            break;
        }
    }
    CHECK_EQ(count, 2);
    if (count == 2) {
        CHECK_STR(evts[0].data, "{\"delta\":\"你好👋世界\"}");
        CHECK_STR(evts[1].data, "x");
    }
    otool_llm_sse_parser_destroy(p);
}

static void test_sse_cap_and_overflow(void)
{
    /* exactly at cap */
    otool_llm_sse_parser_t *p = otool_llm_sse_parser_create(8);
    captured_event_t evts[4];
    int n = feed_all(p, "data: 12345678\n\n", evts, 4);
    CHECK_EQ(n, 1);
    if (n == 1) {
        CHECK_STR(evts[0].data, "12345678");
        CHECK_EQ(evts[0].data_len, 8);
    }
    otool_llm_sse_parser_destroy(p);

    /* over the merged-data cap */
    p = otool_llm_sse_parser_create(8);
    n = feed_all(p, "data: 123456789\n\n", evts, 4);
    CHECK_EQ(n, -1);
    otool_llm_sse_parser_destroy(p);

    /* multi-line merged data over cap */
    p = otool_llm_sse_parser_create(8);
    n = feed_all(p, "data: 1234\ndata: 5678\n\ndata: x\n\n", evts, 4);
    CHECK_EQ(n, -1);
    otool_llm_sse_parser_destroy(p);

    /* a single line longer than cap + headroom is a hard error */
    p = otool_llm_sse_parser_create(4);
    char longline[200];
    memset(longline, 'x', sizeof(longline) - 1);
    longline[sizeof(longline) - 1] = '\0';
    char payload[220];
    snprintf(payload, sizeof(payload), "%s\n\n", longline);
    n = feed_all(p, payload, evts, 4);
    CHECK_EQ(n, -1);
    otool_llm_sse_parser_destroy(p);

    /* event name over cap */
    p = otool_llm_sse_parser_create(1024);
    char big[80];
    memset(big, 'e', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    snprintf(payload, sizeof(payload), "event: %s\ndata: x\n\n", big);
    n = feed_all(p, payload, evts, 4);
    CHECK_EQ(n, -1);
    otool_llm_sse_parser_destroy(p);

    /* id over cap */
    p = otool_llm_sse_parser_create(1024);
    char bigid[600];
    memset(bigid, 'i', sizeof(bigid) - 1);
    bigid[sizeof(bigid) - 1] = '\0';
    char idpayload[700];
    snprintf(idpayload, sizeof(idpayload), "id: %s\ndata: x\n\n", bigid);
    n = feed_all(p, idpayload, evts, 4);
    CHECK_EQ(n, -1);
    otool_llm_sse_parser_destroy(p);
}

static void test_sse_eof_half_event(void)
{
    otool_llm_sse_parser_t *p = otool_llm_sse_parser_create(16384);
    /* clean EOF */
    CHECK(otool_llm_sse_parser_finish(p) == ESP_OK, "clean finish");
    otool_llm_sse_parser_destroy(p);

    /* half line at EOF */
    p = otool_llm_sse_parser_create(16384);
    otool_llm_sse_event_t evt;
    size_t consumed = 0;
    CHECK(otool_llm_sse_parser_feed(p, (const uint8_t *)"data: partial", 13, &evt, &consumed) ==
              OTOOL_LLM_SSE_FEED_OK,
          "feed partial");
    CHECK(otool_llm_sse_parser_finish(p) == OTOOL_LLM_ERR_PROTOCOL_EOF, "half line -> protocol EOF");
    otool_llm_sse_parser_destroy(p);

    /* complete event then EOF is clean */
    p = otool_llm_sse_parser_create(16384);
    captured_event_t cevts[4];
    int n = feed_all(p, "data: done\n\n", cevts, 1);
    CHECK_EQ(n, 1);
    CHECK(otool_llm_sse_parser_finish(p) == ESP_OK, "clean finish after event");
    otool_llm_sse_parser_destroy(p);
}

static void test_sse_crlf_across_feed_boundary(void)
{
    /* P0-3 回归：CR 与 LF 跨 feed 分片时必须等价于单次喂入的 CRLF。
     * 流以空行结束（两个事件均已 dispatch），finish 必须干净。 */
    const char *stream = "data: hello\r\n\r\ndata: world\r\n\r\n";
    size_t len = strlen(stream);
    for (size_t split = 0; split <= len; split++) {
        captured_event_t e1[4], e2[4];
        otool_llm_sse_parser_t *whole = otool_llm_sse_parser_create(16384);
        int n1 = feed_all(whole, stream, e1, 4);
        CHECK(otool_llm_sse_parser_finish(whole) == ESP_OK, "whole clean finish");
        otool_llm_sse_parser_destroy(whole);

        otool_llm_sse_parser_t *split_p = otool_llm_sse_parser_create(16384);
        int n2 = 0;
        if (split > 0) {
            char *part = (char *)malloc(split + 1);
            memcpy(part, stream, split);
            part[split] = '\0';
            n2 = feed_all(split_p, part, e2, 4);
            free(part);
        }
        n2 += feed_all(split_p, stream + split, e2 + n2, 4 - n2);
        esp_err_t finish_ret = otool_llm_sse_parser_finish(split_p);
        otool_llm_sse_parser_destroy(split_p);

        CHECK_EQ(n1, n2);
        for (int i = 0; i < n1 && i < n2; i++) {
            CHECK_STR(e1[i].data, e2[i].data);
        }
        CHECK(finish_ret == ESP_OK, "split finish must be clean at split %d", (int)split);
    }
}

static void test_sse_crlf_mixed(void)
{
    otool_llm_sse_parser_t *p = otool_llm_sse_parser_create(16384);
    captured_event_t evts[4];
    int n = feed_all(p, "data: a\r\n\r\ndata: b\r\rdata: c\n\n", evts, 4);
    CHECK_EQ(n, 3);
    if (n == 3) {
        CHECK_STR(evts[0].data, "a");
        CHECK_STR(evts[1].data, "b");
        CHECK_STR(evts[2].data, "c");
    }
    otool_llm_sse_parser_destroy(p);
}

/* ---- adapter test scaffolding ---- */

static otool_llm_exec_ctx_t *make_ctx(void)
{
    otool_llm_exec_ctx_t *ctx = (otool_llm_exec_ctx_t *)calloc(1, sizeof(*ctx));
    return ctx;
}

static otool_llm_request_view_t make_view(void)
{
    static const otool_llm_request_message_t messages[] = {
        { .role = OTOOL_LLM_ROLE_USER, .text = "你好" },
    };
    otool_llm_request_view_t view = {
        .model = "test-model",
        .instructions = "Be brief.",
        .messages = messages,
        .message_count = 1,
        .max_output_tokens = 64,
        .temperature = 0.7f,
        .temperature_is_set = true,
        .store = false,
    };
    return view;
}

/* collector emit: records events into the ctx's user area via static list */
typedef struct {
    otool_llm_text_event_t events[64];
    int count;
    int cancelled;
} collector_t;

static esp_err_t collector_emit(otool_llm_exec_ctx_t *ctx, const otool_llm_text_event_t *evt)
{
    collector_t *col = (collector_t *)ctx->user_ctx;
    if (ctx->terminal_sent) {
        return ESP_OK;
    }
    col->events[col->count] = *evt;
    /* mirror exec_emit: mark the terminal before invoking the callback */
    switch (evt->type) {
    case OTOOL_LLM_TEXT_EVENT_COMPLETED:
    case OTOOL_LLM_TEXT_EVENT_INCOMPLETE:
    case OTOOL_LLM_TEXT_EVENT_CANCELLED:
    case OTOOL_LLM_TEXT_EVENT_ERROR:
        ctx->terminal_sent = true;
        break;
    default:
        break;
    }
    /* deep-copy the small strings so checks run after feed returns */
    switch (evt->type) {
    case OTOOL_LLM_TEXT_EVENT_TEXT_DELTA:
        col->events[col->count].data.text_delta.data = strdup(evt->data.text_delta.data);
        break;
    case OTOOL_LLM_TEXT_EVENT_ERROR:
        col->events[col->count].data.error.message = strdup(evt->data.error.message);
        if (evt->data.error.provider_code != NULL) {
            col->events[col->count].data.error.provider_code = strdup(evt->data.error.provider_code);
        }
        break;
    case OTOOL_LLM_TEXT_EVENT_INCOMPLETE:
        if (evt->data.incomplete.reason != NULL) {
            col->events[col->count].data.incomplete.reason = strdup(evt->data.incomplete.reason);
        }
        break;
    case OTOOL_LLM_TEXT_EVENT_TOOL_ARGUMENTS_DELTA:
        col->events[col->count].data.tool_arguments_delta.delta =
            strdup(evt->data.tool_arguments_delta.delta);
        break;
    case OTOOL_LLM_TEXT_EVENT_TOOL_CALL_DONE:
        if (evt->data.tool_call_done.arguments != NULL) {
            col->events[col->count].data.tool_call_done.arguments =
                strdup(evt->data.tool_call_done.arguments);
        }
        break;
    case OTOOL_LLM_TEXT_EVENT_TOOL_CALL_STARTED:
        if (evt->data.tool_call_started.call_id != NULL) {
            col->events[col->count].data.tool_call_started.call_id =
                strdup(evt->data.tool_call_started.call_id);
        }
        if (evt->data.tool_call_started.name != NULL) {
            col->events[col->count].data.tool_call_started.name =
                strdup(evt->data.tool_call_started.name);
        }
        break;
    default:
        break;
    }
    col->count++;
    if (col->cancelled) {
        ctx->cancel_requested = true;
    }
    return ESP_OK;
}

static void free_collector(collector_t *col)
{
    for (int i = 0; i < col->count; i++) {
        switch (col->events[i].type) {
        case OTOOL_LLM_TEXT_EVENT_TEXT_DELTA:
            free((void *)col->events[i].data.text_delta.data);
            break;
        case OTOOL_LLM_TEXT_EVENT_ERROR:
            free((void *)col->events[i].data.error.message);
            free((void *)col->events[i].data.error.provider_code);
            break;
        case OTOOL_LLM_TEXT_EVENT_INCOMPLETE:
            free((void *)col->events[i].data.incomplete.reason);
            break;
        case OTOOL_LLM_TEXT_EVENT_TOOL_ARGUMENTS_DELTA:
            free((void *)col->events[i].data.tool_arguments_delta.delta);
            break;
        case OTOOL_LLM_TEXT_EVENT_TOOL_CALL_DONE:
            free((void *)col->events[i].data.tool_call_done.arguments);
            break;
        case OTOOL_LLM_TEXT_EVENT_TOOL_CALL_STARTED:
            free((void *)col->events[i].data.tool_call_started.call_id);
            free((void *)col->events[i].data.tool_call_started.name);
            break;
        default:
            break;
        }
    }
    memset(col, 0, sizeof(*col));
}

static int feed_adapter(otool_llm_protocol_ops_t *ops, otool_llm_exec_ctx_t *ctx,
                        const char *event_name, const char *data)
{
    return ops->on_sse_event(ctx, event_name, data, strlen(data));
}

/* ---- responses adapter tests ---- */

static void test_responses_happy_path(void)
{
    otool_llm_protocol_ops_t *ops = (otool_llm_protocol_ops_t *)&otool_llm_protocol_responses;
    otool_llm_exec_ctx_t *ctx = make_ctx();
    collector_t col = { 0 };
    ctx->user_ctx = &col;
    ctx->emit = collector_emit;
    ctx->request = NULL;

    otool_llm_request_view_t view = make_view();
    ctx->request = &view;

    /* request build */
    char buf[4096];
    size_t len = 0;
    const otool_llm_provider_preset_t *provider = otool_llm_provider_get(OTOOL_LLM_PROVIDER_OPENAI);
    CHECK(ops->build_request(&view, provider, buf, sizeof(buf), &len) == ESP_OK, "build request");
    CHECK(len > 0 && len < sizeof(buf), "built length sane");
    CHECK(strstr(buf, "\"model\":\"test-model\"") != NULL, "model in body");
    CHECK(strstr(buf, "\"stream\":true") != NULL, "stream in body");
    CHECK(strstr(buf, "\"instructions\":\"Be brief.\"") != NULL, "instructions in body");
    CHECK(strstr(buf, "\"max_output_tokens\":64") != NULL, "max_output_tokens in body");
    CHECK(strstr(buf, "\"temperature\":") != NULL, "temperature in body");
    CHECK(strstr(buf, "\"store\":false") != NULL, "store false in body");
    CHECK(strstr(buf, "\"role\":\"user\"") != NULL, "role in body");

    /* created */
    const char *created =
        "{\"type\":\"response.created\",\"response\":{\"id\":\"resp_123\",\"model\":\"test-model\","
        "\"status\":\"in_progress\"}}";
    CHECK(feed_adapter(ops, ctx, "response.created", created) == ESP_OK, "created");
    CHECK_EQ(col.count, 1);
    CHECK(col.events[0].type == OTOOL_LLM_TEXT_EVENT_RESPONSE_STARTED, "started event");
    CHECK_STR(ctx->response_id, "resp_123");
    CHECK_STR(ctx->model, "test-model");

    /* delta */
    CHECK(feed_adapter(ops, ctx, "response.output_text.delta",
                       "{\"type\":\"response.output_text.delta\",\"delta\":\"你好\"}") == ESP_OK,
          "delta 1");
    CHECK(feed_adapter(ops, ctx, "response.output_text.delta",
                       "{\"type\":\"response.output_text.delta\",\"delta\":\" world\"}") == ESP_OK,
          "delta 2");
    CHECK(feed_adapter(ops, ctx, "response.output_text.delta",
                       "{\"type\":\"response.output_text.delta\",\"delta\":\"\"}") == ESP_OK,
          "empty delta");
    CHECK_EQ(col.count, 4);
    CHECK(col.events[1].type == OTOOL_LLM_TEXT_EVENT_TEXT_DELTA, "delta event");
    CHECK_STR(col.events[1].data.text_delta.data, "你好");
    CHECK_EQ(col.events[1].data.text_delta.data_len, 6);
    CHECK_STR(col.events[3].data.text_delta.data, "");
    CHECK_EQ(col.events[3].data.text_delta.data_len, 0);

    /* done: must NOT resend full text */
    CHECK(feed_adapter(ops, ctx, "response.output_text.done",
                       "{\"type\":\"response.output_text.done\",\"text\":\"你好 world\"}") == ESP_OK,
          "done");
    CHECK_EQ(col.count, 5);
    CHECK(col.events[4].type == OTOOL_LLM_TEXT_EVENT_TEXT_DONE, "done event");

    /* completed with usage */
    CHECK(feed_adapter(ops, ctx, "response.completed",
                       "{\"type\":\"response.completed\",\"response\":{\"id\":\"resp_123\","
                       "\"usage\":{\"input_tokens\":12,\"output_tokens\":34,\"total_tokens\":46}}}") ==
              ESP_OK,
          "completed");
    CHECK_EQ(col.count, 7);
    CHECK(col.events[5].type == OTOOL_LLM_TEXT_EVENT_USAGE, "usage event");
    CHECK_EQ(col.events[5].data.usage.input_tokens, 12);
    CHECK_EQ(col.events[5].data.usage.output_tokens, 34);
    CHECK_EQ(col.events[5].data.usage.total_tokens, 46);
    CHECK(col.events[6].type == OTOOL_LLM_TEXT_EVENT_COMPLETED, "completed event");
    CHECK(ctx->terminal_sent, "terminal set");

    /* events after terminal are ignored */
    CHECK(feed_adapter(ops, ctx, "response.output_text.delta",
                       "{\"type\":\"response.output_text.delta\",\"delta\":\"late\"}") == ESP_OK,
          "late delta");
    CHECK_EQ(col.count, 7);

    free_collector(&col);
    free(ctx);
}

static void test_responses_incomplete(void)
{
    otool_llm_protocol_ops_t *ops = (otool_llm_protocol_ops_t *)&otool_llm_protocol_responses;
    otool_llm_exec_ctx_t *ctx = make_ctx();
    collector_t col = { 0 };
    ctx->user_ctx = &col;
    ctx->emit = collector_emit;

    CHECK(feed_adapter(ops, ctx, "response.incomplete",
                       "{\"type\":\"response.incomplete\",\"response\":{\"id\":\"r\","
                       "\"incomplete_details\":{\"reason\":\"max_output_tokens\"}}}") == ESP_OK,
          "incomplete");
    CHECK_EQ(col.count, 1);
    CHECK(col.events[0].type == OTOOL_LLM_TEXT_EVENT_INCOMPLETE, "incomplete event");
    CHECK_STR(col.events[0].data.incomplete.reason, "max_output_tokens");
    CHECK(ctx->terminal_sent, "terminal set");

    free_collector(&col);
    free(ctx);
}

static void test_responses_failed_and_error_events(void)
{
    otool_llm_protocol_ops_t *ops = (otool_llm_protocol_ops_t *)&otool_llm_protocol_responses;
    otool_llm_exec_ctx_t *ctx = make_ctx();
    collector_t col = { 0 };
    ctx->user_ctx = &col;
    ctx->emit = collector_emit;

    CHECK(feed_adapter(ops, ctx, "response.failed",
                       "{\"type\":\"response.failed\",\"response\":{\"id\":\"r\",\"error\":"
                       "{\"code\":\"server_error\",\"message\":\"boom\"}}}") != ESP_OK,
          "failed");
    CHECK_EQ(col.count, 1);
    CHECK(col.events[0].type == OTOOL_LLM_TEXT_EVENT_ERROR, "error event");
    CHECK_EQ(col.events[0].data.error.code, OTOOL_LLM_ERR_PROVIDER);
    CHECK_STR(col.events[0].data.error.message, "boom");
    CHECK_STR(col.events[0].data.error.provider_code, "server_error");
    CHECK(ctx->terminal_sent, "terminal set");
    free_collector(&col);
    free(ctx);

    /* separate error event shape */
    ctx = make_ctx();
    col = (collector_t){ 0 };
    ctx->user_ctx = &col;
    ctx->emit = collector_emit;
    CHECK(feed_adapter(ops, ctx, "error",
                       "{\"code\":\"rate_limit_exceeded\",\"message\":\"slow down\","
                       "\"request_id\":\"req_9\"}") != ESP_OK,
          "error event");
    CHECK_EQ(col.count, 1);
    CHECK(col.events[0].type == OTOOL_LLM_TEXT_EVENT_ERROR, "error event");
    CHECK_STR(col.events[0].data.error.message, "slow down");
    CHECK_STR(col.events[0].data.error.provider_code, "rate_limit_exceeded");
    CHECK_STR(ctx->request_id, "req_9");
    free_collector(&col);
    free(ctx);
}

static void test_responses_unknown_events_tolerated(void)
{
    otool_llm_protocol_ops_t *ops = (otool_llm_protocol_ops_t *)&otool_llm_protocol_responses;
    otool_llm_exec_ctx_t *ctx = make_ctx();
    collector_t col = { 0 };
    ctx->user_ctx = &col;
    ctx->emit = collector_emit;

    CHECK(feed_adapter(ops, ctx, "response.output_item.added",
                       "{\"type\":\"response.output_item.added\",\"output_index\":0}") == ESP_OK,
          "unknown 1");
    CHECK(feed_adapter(ops, ctx, "response.custom_event",
                       "{\"type\":\"response.custom_event\",\"delta\":\"{\\\"a\\\"\"}") == ESP_OK,
          "unknown 2");
    CHECK(feed_adapter(ops, ctx, "response.reasoning_summary_text.delta",
                       "{\"type\":\"response.reasoning_summary_text.delta\",\"delta\":\"think\"}") ==
              ESP_OK,
          "unknown 3");
    CHECK_EQ(col.count, 0);
    free_collector(&col);
    free(ctx);
}

static void test_responses_ark_done_marker(void)
{
    /* 方舟差异（§15.6）：Responses 流末尾也发 data: [DONE]。
     * adapter 必须忽略它：不报 JSON 错误、不伪造完成事件。 */
    otool_llm_protocol_ops_t *ops = (otool_llm_protocol_ops_t *)&otool_llm_protocol_responses;
    otool_llm_exec_ctx_t *ctx = make_ctx();
    collector_t col = { 0 };
    ctx->user_ctx = &col;
    ctx->emit = collector_emit;

    CHECK(feed_adapter(ops, ctx, "message", "[DONE]") == ESP_OK, "ark [DONE] ignored");
    CHECK_EQ(col.count, 0);
    CHECK(!ctx->terminal_sent, "ark [DONE] does not terminate");

    /* [DONE] 到达前先有完整 created → completed 序列仍然正常 */
    CHECK(feed_adapter(ops, ctx, "response.created",
                       "{\"type\":\"response.created\",\"response\":{\"id\":\"r\"}}") == ESP_OK,
          "created");
    CHECK(feed_adapter(ops, ctx, "response.completed",
                       "{\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}") ==
              ESP_OK,
          "completed");
    CHECK(feed_adapter(ops, ctx, "message", "[DONE]") == ESP_OK, "trailing [DONE] ignored");
    CHECK_EQ(col.count, 2);
    CHECK(ctx->terminal_sent, "terminal from completed");

    free_collector(&col);
    free(ctx);
}

static void test_responses_bad_json_and_types(void)
{
    otool_llm_protocol_ops_t *ops = (otool_llm_protocol_ops_t *)&otool_llm_protocol_responses;
    otool_llm_exec_ctx_t *ctx = make_ctx();
    collector_t col = { 0 };
    ctx->user_ctx = &col;
    ctx->emit = collector_emit;

    /* invalid JSON */
    CHECK(feed_adapter(ops, ctx, "response.output_text.delta", "{not json") != ESP_OK, "bad json");
    CHECK_EQ(col.count, 1);
    CHECK(col.events[0].type == OTOOL_LLM_TEXT_EVENT_ERROR, "error event");
    CHECK_EQ(col.events[0].data.error.code, OTOOL_LLM_ERR_JSON);
    CHECK(ctx->terminal_sent, "terminal set");
    free_collector(&col);
    free(ctx);

    /* wrong type for delta */
    ctx = make_ctx();
    col = (collector_t){ 0 };
    ctx->user_ctx = &col;
    ctx->emit = collector_emit;
    CHECK(feed_adapter(ops, ctx, "response.output_text.delta",
                       "{\"type\":\"response.output_text.delta\",\"delta\":42}") != ESP_OK,
          "delta wrong type");
    CHECK_EQ(col.count, 1);
    CHECK(col.events[0].type == OTOOL_LLM_TEXT_EVENT_ERROR, "error event");
    free_collector(&col);
    free(ctx);
}

static void test_responses_on_eof(void)
{
    otool_llm_protocol_ops_t *ops = (otool_llm_protocol_ops_t *)&otool_llm_protocol_responses;
    otool_llm_exec_ctx_t *ctx = make_ctx();
    collector_t col = { 0 };
    ctx->user_ctx = &col;
    ctx->emit = collector_emit;
    CHECK(ops->on_eof(ctx) != ESP_OK, "eof without terminal");
    CHECK_EQ(col.count, 1);
    CHECK(col.events[0].type == OTOOL_LLM_TEXT_EVENT_ERROR, "error event");
    CHECK_EQ(col.events[0].data.error.code, OTOOL_LLM_ERR_PROTOCOL_EOF);
    free_collector(&col);
    free(ctx);
}

/* ---- WP2: responses tool calling tests ---- */

static void test_responses_tool_call_flow(void)
{
    /* Ark 真实事件结构（ark_tool_probe.py 录制，字段脱敏）：
     * output_item.added(function_call) → function_call_arguments.delta ×3（含空串）
     * → function_call_arguments.done → output_item.done(function_call) → completed */
    otool_llm_protocol_ops_t *ops = (otool_llm_protocol_ops_t *)&otool_llm_protocol_responses;
    otool_llm_exec_ctx_t *ctx = make_ctx();
    collector_t col = { 0 };
    ctx->user_ctx = &col;
    ctx->emit = collector_emit;

    const char *added =
        "{\"type\":\"response.output_item.added\",\"output_index\":1,"
        "\"item\":{\"id\":\"item_abc\",\"call_id\":\"call_xyz\",\"name\":\"get_weather\","
        "\"type\":\"function_call\",\"status\":\"in_progress\"}}";
    CHECK(feed_adapter(ops, ctx, "response.output_item.added", added) == ESP_OK, "tool item added");
    CHECK_EQ(col.count, 1);
    CHECK(col.events[0].type == OTOOL_LLM_TEXT_EVENT_TOOL_CALL_STARTED, "tool started");
    CHECK_EQ(col.events[0].data.tool_call_started.output_index, 1);
    CHECK_STR(col.events[0].data.tool_call_started.call_id, "call_xyz");
    CHECK_STR(col.events[0].data.tool_call_started.name, "get_weather");
    CHECK_STR(col.events[0].data.tool_call_started.item_id, "item_abc");

    /* 空 delta + 中文分片 */
    CHECK(feed_adapter(ops, ctx, "response.function_call_arguments.delta",
                       "{\"type\":\"response.function_call_arguments.delta\",\"delta\":\"\","
                       "\"item_id\":\"item_abc\",\"output_index\":1}") == ESP_OK, "empty delta");
    CHECK(feed_adapter(ops, ctx, "response.function_call_arguments.delta",
                       "{\"type\":\"response.function_call_arguments.delta\",\"delta\":\"{\\\"city\\\": \\\"\","
                       "\"item_id\":\"item_abc\",\"output_index\":1}") == ESP_OK, "delta 1");
    CHECK(feed_adapter(ops, ctx, "response.function_call_arguments.delta",
                       "{\"type\":\"response.function_call_arguments.delta\",\"delta\":\"北京\","
                       "\"item_id\":\"item_abc\",\"output_index\":1}") == ESP_OK, "delta 2");
    CHECK(feed_adapter(ops, ctx, "response.function_call_arguments.delta",
                       "{\"type\":\"response.function_call_arguments.delta\",\"delta\":\"\\\"}\","
                       "\"item_id\":\"item_abc\",\"output_index\":1}") == ESP_OK, "delta 3");
    /* started(0) + 空 delta(1) + d1(2) + d2(3) + d3(4) = 5 */
    CHECK_EQ(col.count, 5);
    CHECK(col.events[1].type == OTOOL_LLM_TEXT_EVENT_TOOL_ARGUMENTS_DELTA, "args delta 1");
    CHECK_EQ(col.events[1].data.tool_arguments_delta.delta_len, 0);
    CHECK_STR(col.events[3].data.tool_arguments_delta.delta, "北京");
    CHECK_EQ(col.events[3].data.tool_arguments_delta.delta_len, 6);
    CHECK_STR(col.events[4].data.tool_arguments_delta.delta, "\"}");
    CHECK_EQ(col.events[4].data.tool_arguments_delta.delta_len, 2);

    /* done：完整 arguments 与累计一致（{"city": "北京"} = 10+6+2 = 18 字节） */
    CHECK(feed_adapter(ops, ctx, "response.function_call_arguments.done",
                       "{\"type\":\"response.function_call_arguments.done\","
                       "\"arguments\":\"{\\\"city\\\": \\\"北京\\\"}\","
                       "\"item_id\":\"item_abc\",\"output_index\":1}") == ESP_OK, "args done");
    CHECK_EQ(col.count, 6);
    CHECK(col.events[5].type == OTOOL_LLM_TEXT_EVENT_TOOL_CALL_DONE, "tool done");
    CHECK_STR(col.events[5].data.tool_call_done.arguments, "{\"city\": \"北京\"}");
    CHECK_EQ(col.events[5].data.tool_call_done.arguments_len, 18);
    CHECK_STR(col.events[5].data.tool_call_done.name, "get_weather");

    /* output_item.done 回收槽位 */
    CHECK(feed_adapter(ops, ctx, "response.output_item.done",
                       "{\"type\":\"response.output_item.done\",\"output_index\":1,"
                       "\"item\":{\"arguments\":\"{\\\"city\\\": \\\"北京\\\"}\",\"call_id\":\"call_xyz\","
                       "\"name\":\"get_weather\",\"type\":\"function_call\",\"status\":\"completed\"}}") ==
              ESP_OK,
          "item done");
    CHECK_EQ(col.count, 6); /* 无新事件 */

    /* completed 正常终止 */
    CHECK(feed_adapter(ops, ctx, "response.completed",
                       "{\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}") ==
              ESP_OK,
          "completed");
    CHECK(col.events[6].type == OTOOL_LLM_TEXT_EVENT_COMPLETED, "completed event");
    CHECK(ctx->terminal_sent, "terminal set");

    free_collector(&col);
    free(ctx);
}

static void test_responses_tool_arguments_mismatch(void)
{
    /* done 的完整 arguments 与累计 delta 不一致 → 协议错误 */
    otool_llm_protocol_ops_t *ops = (otool_llm_protocol_ops_t *)&otool_llm_protocol_responses;
    otool_llm_exec_ctx_t *ctx = make_ctx();
    collector_t col = { 0 };
    ctx->user_ctx = &col;
    ctx->emit = collector_emit;

    CHECK(feed_adapter(ops, ctx, "response.output_item.added",
                       "{\"type\":\"response.output_item.added\",\"output_index\":0,"
                       "\"item\":{\"call_id\":\"c1\",\"name\":\"t\",\"type\":\"function_call\"}}") == ESP_OK,
          "added");
    CHECK(feed_adapter(ops, ctx, "response.function_call_arguments.delta",
                       "{\"type\":\"response.function_call_arguments.delta\",\"delta\":\"{\\\"a\\\":1}\","
                       "\"output_index\":0}") == ESP_OK, "delta");
    /* 不一致的 done → ERROR(PROTOCOL)；事件：started(0)+delta(1)+error(2) */
    CHECK(feed_adapter(ops, ctx, "response.function_call_arguments.done",
                       "{\"type\":\"response.function_call_arguments.done\",\"arguments\":\"{\\\"b\\\":2}\","
                       "\"output_index\":0}") != ESP_OK, "mismatch rejected");
    CHECK_EQ(col.count, 3);
    CHECK(col.events[2].type == OTOOL_LLM_TEXT_EVENT_ERROR, "error event");
    CHECK_EQ(col.events[2].data.error.code, OTOOL_LLM_ERR_PROTOCOL);
    CHECK(ctx->terminal_sent, "terminal set");
    free_collector(&col);
    free(ctx);
}

static void test_responses_tool_half_call_at_terminal(void)
{
    /* completed 时仍有未完成工具调用 → 协议错误 */
    otool_llm_protocol_ops_t *ops = (otool_llm_protocol_ops_t *)&otool_llm_protocol_responses;
    otool_llm_exec_ctx_t *ctx = make_ctx();
    collector_t col = { 0 };
    ctx->user_ctx = &col;
    ctx->emit = collector_emit;

    CHECK(feed_adapter(ops, ctx, "response.output_item.added",
                       "{\"type\":\"response.output_item.added\",\"output_index\":0,"
                       "\"item\":{\"call_id\":\"c1\",\"name\":\"t\",\"type\":\"function_call\"}}") == ESP_OK,
          "added");
    CHECK(feed_adapter(ops, ctx, "response.completed",
                       "{\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}") != ESP_OK,
          "completed with half call rejected");
    CHECK(col.events[1].type == OTOOL_LLM_TEXT_EVENT_ERROR, "error event");
    CHECK_EQ(col.events[1].data.error.code, OTOOL_LLM_ERR_PROTOCOL);
    free_collector(&col);
    free(ctx);
}

static void test_tool_identity_and_index_bounds(void)
{
    char long_name[80];
    memset(long_name, 'x', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    char json[512];

    otool_llm_protocol_ops_t *responses =
        (otool_llm_protocol_ops_t *)&otool_llm_protocol_responses;
    otool_llm_exec_ctx_t *ctx = make_ctx();
    collector_t col = { 0 };
    ctx->user_ctx = &col;
    ctx->emit = collector_emit;
    snprintf(json, sizeof(json),
             "{\"type\":\"response.output_item.added\",\"output_index\":0,"
             "\"item\":{\"call_id\":\"c1\",\"name\":\"%s\","
             "\"type\":\"function_call\"}}",
             long_name);
    CHECK(feed_adapter(responses, ctx, "response.output_item.added", json) != ESP_OK,
          "overlong Responses tool name rejected instead of truncated");
    CHECK(col.count == 1 && col.events[0].type == OTOOL_LLM_TEXT_EVENT_ERROR,
          "overlong Responses identity emits terminal error");
    free_collector(&col);
    free(ctx);

    ctx = make_ctx();
    col = (collector_t){ 0 };
    ctx->user_ctx = &col;
    ctx->emit = collector_emit;
    CHECK(feed_adapter(responses, ctx, "response.output_item.added",
                       "{\"type\":\"response.output_item.added\",\"output_index\":-1,"
                       "\"item\":{\"call_id\":\"c1\",\"name\":\"t\","
                       "\"type\":\"function_call\"}}") != ESP_OK,
          "negative Responses output_index rejected");
    free_collector(&col);
    free(ctx);

    otool_llm_protocol_ops_t *chat =
        (otool_llm_protocol_ops_t *)&otool_llm_protocol_chat;
    ctx = make_ctx();
    col = (collector_t){ 0 };
    ctx->user_ctx = &col;
    ctx->emit = collector_emit;
    CHECK(feed_adapter(chat, ctx, "message",
                       "{\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{"
                       "\"id\":\"c1\",\"function\":{\"name\":\"t\","
                       "\"arguments\":\"{}\"}}]},\"finish_reason\":null}]}") != ESP_OK,
          "Chat tool call without index rejected");
    free_collector(&col);
    free(ctx);

    ctx = make_ctx();
    col = (collector_t){ 0 };
    ctx->user_ctx = &col;
    ctx->emit = collector_emit;
    CHECK(feed_adapter(chat, ctx, "message",
                       "{\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{"
                       "\"index\":0,\"id\":\"c1\",\"function\":{\"name\":\"t\","
                       "\"arguments\":\"{}\"}}]},\"finish_reason\":null}]}") == ESP_OK,
          "Chat tool call starts");
    CHECK(feed_adapter(chat, ctx, "message", "[DONE]") != ESP_OK,
          "Chat active tool requires finish_reason=tool_calls");
    CHECK(ctx->terminal_sent, "malformed Chat tool terminal error");
    free_collector(&col);
    free(ctx);
}

static void test_responses_build_request_with_tools(void)
{
    otool_llm_protocol_ops_t *ops = (otool_llm_protocol_ops_t *)&otool_llm_protocol_responses;
    otool_llm_request_view_t view = make_view();
    static const otool_llm_tool_definition_t tools[] = {
        {
            .struct_size = sizeof(otool_llm_tool_definition_t),
            .name = "get_weather",
            .description = "Get current weather for a city",
            .parameters_json_schema =
                "{\"type\":\"object\",\"properties\":{\"city\":{\"type\":\"string\"}},"
                "\"required\":[\"city\"],\"additionalProperties\":false}",
            .strict = true,
        },
    };
    static const otool_llm_tool_output_t outputs[] = {
        { .call_id = "call_xyz", .output = "{\"ok\":true,\"result\":{\"temp\":25}}" },
    };
    view.tools = tools;
    view.tool_count = 1;
    view.tool_outputs = outputs;
    view.tool_output_count = 1;

    char buf[8192];
    size_t len = 0;
    const otool_llm_provider_preset_t *provider = otool_llm_provider_get(OTOOL_LLM_PROVIDER_OPENAI);
    CHECK(ops->build_request(&view, provider, buf, sizeof(buf), &len) == ESP_OK, "build with tools");
    CHECK(strstr(buf, "\"tools\":[{\"type\":\"function\",\"name\":\"get_weather\"") != NULL,
          "tool def in body");
    CHECK(strstr(buf, "\"tool_choice\":\"auto\"") != NULL, "tool_choice auto");
    CHECK(strstr(buf, "\"parallel_tool_calls\":false") != NULL, "parallel false");
    CHECK(strstr(buf, "\"type\":\"function_call_output\"") != NULL, "tool output item");
    CHECK(strstr(buf, "\"call_id\":\"call_xyz\"") != NULL, "output call_id");
}

/* ---- WP3: tool registry + schema validation tests ---- */

static const char *good_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"city\":{\"type\":\"string\"},"
    "\"days\":{\"type\":\"integer\",\"enum\":[1,3,7]},"
    "\"verbose\":{\"type\":\"boolean\"},"
    "\"note\":{\"type\":\"null\"}"
    "},\"required\":[\"city\"],\"additionalProperties\":false}";

static void test_registry_basic(void)
{
    otool_llm_tool_registry_handle_t reg = NULL;
    CHECK(otool_llm_tool_registry_create(&reg) == ESP_OK, "reg create");
    CHECK(reg != NULL, "reg non-null");

    otool_llm_tool_definition_t tool = {};
    tool.struct_size = sizeof(tool);
    tool.name = "get_weather";
    tool.description = "weather";
    tool.parameters_json_schema = good_schema;
    tool.strict = false; /* optional properties are intentionally accepted in this baseline case */
    tool.flags = OTOOL_LLM_TOOL_READ_ONLY;
    CHECK(otool_llm_tool_registry_add(reg, &tool) == ESP_OK, "add tool");
    CHECK_EQ(otool_llm_tool_registry_count(reg), 1);

    /* 重复名称拒绝 */
    CHECK(otool_llm_tool_registry_add(reg, &tool) == ESP_ERR_INVALID_ARG, "dup name");

    /* 非法名称拒绝 */
    tool.name = "bad name!";
    CHECK(otool_llm_tool_registry_add(reg, &tool) == ESP_ERR_INVALID_ARG, "bad name");
    tool.name = "tool_ok";
    CHECK(otool_llm_tool_registry_add(reg, &tool) == ESP_OK, "second tool");
    CHECK_EQ(otool_llm_tool_registry_count(reg), 2);

    /* 注册后原字符串可释放（深拷贝） */
    tool.name = "get_weather";
    const otool_llm_tool_definition_t *found = otool_llm_tool_registry_find(reg, "get_weather");
    CHECK(found != NULL, "find");
    CHECK_STR(found->name, "get_weather");
    CHECK(found->parameters_json_schema != good_schema, "schema deep-copied");

    /* seal 后 add 拒绝 */
    CHECK(otool_llm_tool_registry_seal(reg) == ESP_OK, "seal");
    CHECK(otool_llm_tool_registry_add(reg, &tool) == ESP_ERR_INVALID_STATE, "add after seal");

    otool_llm_tool_registry_destroy(reg);
}

static void test_registry_schema_rejection(void)
{
    otool_llm_tool_registry_handle_t reg = NULL;
    otool_llm_tool_registry_create(&reg);

    otool_llm_tool_definition_t tool = {};
    tool.struct_size = sizeof(tool);
    tool.name = "bad_schema";
    tool.parameters_json_schema = NULL;
    CHECK(otool_llm_tool_registry_add(reg, &tool) == OTOOL_LLM_ERR_TOOL_SCHEMA, "null schema");

    /* 非法 JSON */
    tool.parameters_json_schema = "{not json";
    CHECK(otool_llm_tool_registry_add(reg, &tool) == OTOOL_LLM_ERR_TOOL_SCHEMA, "bad json schema");

    /* 顶层非 object */
    tool.parameters_json_schema = "{\"type\":\"string\"}";
    CHECK(otool_llm_tool_registry_add(reg, &tool) == OTOOL_LLM_ERR_TOOL_SCHEMA, "non-object top");

    /* additionalProperties: true */
    tool.parameters_json_schema =
        "{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"string\"}},"
        "\"additionalProperties\":true}";
    CHECK(otool_llm_tool_registry_add(reg, &tool) == OTOOL_LLM_ERR_TOOL_SCHEMA, "ap true");

    /* required 引用未声明字段 */
    tool.parameters_json_schema =
        "{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"string\"}},"
        "\"required\":[\"nope\"]}";
    CHECK(otool_llm_tool_registry_add(reg, &tool) == OTOOL_LLM_ERR_TOOL_SCHEMA, "required unknown");

    /* 不支持的属性类型 */
    tool.parameters_json_schema =
        "{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"array\"}}}";
    CHECK(otool_llm_tool_registry_add(reg, &tool) == OTOOL_LLM_ERR_TOOL_SCHEMA, "array prop");

    /* 未实现关键字 */
    tool.parameters_json_schema =
        "{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"string\",\"minLength\":2}}}";
    CHECK(otool_llm_tool_registry_add(reg, &tool) == OTOOL_LLM_ERR_TOOL_SCHEMA, "minLength");

    /* 嵌套 object 属性 */
    tool.parameters_json_schema =
        "{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"object\",\"properties\":{}}}}";
    CHECK(otool_llm_tool_registry_add(reg, &tool) == OTOOL_LLM_ERR_TOOL_SCHEMA, "nested object");

    /* strict：必须显式 additionalProperties:false，且所有 properties 均 required。 */
    tool.strict = true;
    tool.parameters_json_schema =
        "{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"string\"}},"
        "\"required\":[]}";
    CHECK(otool_llm_tool_registry_add(reg, &tool) == OTOOL_LLM_ERR_TOOL_SCHEMA,
          "strict requires additionalProperties false");
    tool.parameters_json_schema =
        "{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"string\"}},"
        "\"required\":[],\"additionalProperties\":false}";
    CHECK(otool_llm_tool_registry_add(reg, &tool) == OTOOL_LLM_ERR_TOOL_SCHEMA,
          "strict requires every property");
    tool.name = "strict_ok";
    tool.parameters_json_schema =
        "{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"string\"}},"
        "\"required\":[\"a\"],\"additionalProperties\":false}";
    CHECK(otool_llm_tool_registry_add(reg, &tool) == ESP_OK, "strict schema accepted");

    otool_llm_tool_registry_destroy(reg);
}

static void test_arguments_validation(void)
{
    otool_llm_tool_definition_t tool = {};
    tool.parameters_json_schema = good_schema;

    /* 合法 */
    CHECK(otool_llm_tool_arguments_validate(&tool, "{\"city\":\"北京\"}",
                                            strlen("{\"city\":\"北京\"}")) == ESP_OK,
          "valid args");
    CHECK(otool_llm_tool_arguments_validate(&tool, "{\"city\":\"x\",\"days\":3}",
                                            strlen("{\"city\":\"x\",\"days\":3}")) == ESP_OK,
          "valid args 2");
    CHECK(otool_llm_tool_arguments_validate(&tool, "{\"city\":\"x\",\"verbose\":true}",
                                            strlen("{\"city\":\"x\",\"verbose\":true}")) == ESP_OK,
          "valid args 3");
    CHECK(otool_llm_tool_arguments_validate(&tool, "{\"city\":\"x\",\"note\":null}",
                                            strlen("{\"city\":\"x\",\"note\":null}")) == ESP_OK,
          "null type ok");

    /* 缺 required */
    CHECK(otool_llm_tool_arguments_validate(&tool, "{\"days\":3}", strlen("{\"days\":3}")) ==
              OTOOL_LLM_ERR_TOOL_ARGUMENTS,
          "missing required");
    /* 未知字段 */
    CHECK(otool_llm_tool_arguments_validate(&tool, "{\"city\":\"x\",\"evil\":1}",
                                            strlen("{\"city\":\"x\",\"evil\":1}")) ==
              OTOOL_LLM_ERR_TOOL_ARGUMENTS,
          "unknown field");
    /* 类型错误 */
    CHECK(otool_llm_tool_arguments_validate(&tool, "{\"city\":123}", strlen("{\"city\":123}")) ==
              OTOOL_LLM_ERR_TOOL_ARGUMENTS,
          "wrong type");
    /* 非 object arguments */
    CHECK(otool_llm_tool_arguments_validate(&tool, "[1,2]", 5) == OTOOL_LLM_ERR_TOOL_ARGUMENTS,
          "array args");
    /* enum 不匹配 */
    CHECK(otool_llm_tool_arguments_validate(&tool, "{\"city\":\"x\",\"days\":2}",
                                            strlen("{\"city\":\"x\",\"days\":2}")) ==
              OTOOL_LLM_ERR_TOOL_ARGUMENTS,
          "enum mismatch");
}

/* ---- chat adapter tests ---- */

static void test_chat_happy_path(void)
{
    otool_llm_protocol_ops_t *ops = (otool_llm_protocol_ops_t *)&otool_llm_protocol_chat;
    otool_llm_exec_ctx_t *ctx = make_ctx();
    collector_t col = { 0 };
    ctx->user_ctx = &col;
    ctx->emit = collector_emit;

    otool_llm_request_view_t view = make_view();
    ctx->request = &view;

    char buf[4096];
    size_t len = 0;
    const otool_llm_provider_preset_t *provider = otool_llm_provider_get(OTOOL_LLM_PROVIDER_OPENAI);
    CHECK(ops->build_request(&view, provider, buf, sizeof(buf), &len) == ESP_OK, "build chat request");
    CHECK(strstr(buf, "\"stream\":true") != NULL, "stream true");
    CHECK(strstr(buf, "\"stream_options\":{\"include_usage\":true}") != NULL, "stream options (openai)");
    CHECK(strstr(buf, "\"max_completion_tokens\":64") != NULL, "max_completion_tokens (openai)");
    CHECK(strstr(buf, "\"messages\"") != NULL, "messages");
    CHECK(strstr(buf, "\"instructions\":\"Be brief.\"") == NULL, "instructions NOT in chat body");

    /* ark: max_tokens, no stream_options */
    provider = otool_llm_provider_get(OTOOL_LLM_PROVIDER_VOLCENGINE_ARK);
    CHECK(ops->build_request(&view, provider, buf, sizeof(buf), &len) == ESP_OK, "build ark chat request");
    CHECK(strstr(buf, "\"max_tokens\":64") != NULL, "max_tokens (ark)");
    CHECK(strstr(buf, "\"stream_options\"") == NULL, "no stream_options (ark)");

    /* role chunk + delta chunks */
    CHECK(feed_adapter(ops, ctx, "message",
                       "{\"id\":\"chatcmpl-1\",\"object\":\"chat.completion.chunk\","
                       "\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\"},"
                       "\"finish_reason\":null}]}") == ESP_OK,
          "role chunk");
    CHECK_EQ(col.count, 0);

    CHECK(feed_adapter(ops, ctx, "message",
                       "{\"id\":\"chatcmpl-1\",\"choices\":[{\"index\":0,"
                       "\"delta\":{\"content\":\"Hello\"},\"finish_reason\":null}]}") == ESP_OK,
          "delta 1");
    CHECK(feed_adapter(ops, ctx, "message",
                       "{\"id\":\"chatcmpl-1\",\"choices\":[{\"index\":0,"
                       "\"delta\":{\"content\":\"\"},\"finish_reason\":null}]}") == ESP_OK,
          "empty delta");
    CHECK_EQ(col.count, 2);
    CHECK(col.events[0].type == OTOOL_LLM_TEXT_EVENT_TEXT_DELTA, "delta");
    CHECK_STR(col.events[0].data.text_delta.data, "Hello");
    CHECK(col.events[1].type == OTOOL_LLM_TEXT_EVENT_TEXT_DELTA, "empty delta emitted");
    CHECK_EQ(col.events[1].data.text_delta.data_len, 0);

    /* finish_reason + usage chunk */
    CHECK(feed_adapter(ops, ctx, "message",
                       "{\"id\":\"chatcmpl-1\",\"choices\":[{\"index\":0,\"delta\":{},"
                       "\"finish_reason\":\"stop\"}],"
                       "\"usage\":{\"prompt_tokens\":5,\"completion_tokens\":7,\"total_tokens\":12}}") ==
              ESP_OK,
          "usage chunk");
    CHECK_EQ(col.count, 3);
    CHECK(col.events[2].type == OTOOL_LLM_TEXT_EVENT_USAGE, "usage");
    CHECK_EQ(col.events[2].data.usage.input_tokens, 5);
    CHECK_EQ(col.events[2].data.usage.output_tokens, 7);
    CHECK_EQ(col.events[2].data.usage.total_tokens, 12);
    CHECK_STR(ctx->proto.chat.finish_reason, "stop");

    /* [DONE] */
    CHECK(feed_adapter(ops, ctx, "message", "[DONE]") == ESP_OK, "done");
    CHECK_EQ(col.count, 4);
    CHECK(col.events[3].type == OTOOL_LLM_TEXT_EVENT_COMPLETED, "completed");
    CHECK(ctx->terminal_sent, "terminal set");

    free_collector(&col);
    free(ctx);
}

static void test_chat_multiple_choices_rejected(void)
{
    otool_llm_protocol_ops_t *ops = (otool_llm_protocol_ops_t *)&otool_llm_protocol_chat;
    otool_llm_exec_ctx_t *ctx = make_ctx();
    collector_t col = { 0 };
    ctx->user_ctx = &col;
    ctx->emit = collector_emit;

    CHECK(feed_adapter(ops, ctx, "message",
                       "{\"choices\":[{\"index\":0,\"delta\":{\"content\":\"a\"}},"
                       "{\"index\":1,\"delta\":{\"content\":\"b\"}}]}") != ESP_OK,
          "multiple choices rejected");
    CHECK_EQ(col.count, 1);
    CHECK(col.events[0].type == OTOOL_LLM_TEXT_EVENT_ERROR, "error");
    CHECK_EQ(col.events[0].data.error.code, OTOOL_LLM_ERR_UNSUPPORTED);
    free_collector(&col);
    free(ctx);
}

static void test_chat_error_event_and_done_eof(void)
{
    otool_llm_protocol_ops_t *ops = (otool_llm_protocol_ops_t *)&otool_llm_protocol_chat;
    otool_llm_exec_ctx_t *ctx = make_ctx();
    collector_t col = { 0 };
    ctx->user_ctx = &col;
    ctx->emit = collector_emit;

    /* mid-stream error object */
    CHECK(feed_adapter(ops, ctx, "message",
                       "{\"error\":{\"message\":\"invalid api key\",\"code\":\"401\"}}") != ESP_OK,
          "mid-stream error");
    CHECK_EQ(col.count, 1);
    CHECK(col.events[0].type == OTOOL_LLM_TEXT_EVENT_ERROR, "error");
    CHECK_STR(col.events[0].data.error.message, "invalid api key");
    CHECK(ctx->terminal_sent, "terminal set");
    free_collector(&col);
    free(ctx);

    /* eof without [DONE] -> protocol EOF */
    ctx = make_ctx();
    col = (collector_t){ 0 };
    ctx->user_ctx = &col;
    ctx->emit = collector_emit;
    CHECK(feed_adapter(ops, ctx, "message",
                       "{\"choices\":[{\"index\":0,\"delta\":{\"content\":\"hi\"}}]}") == ESP_OK,
          "delta");
    CHECK(ops->on_eof(ctx) != ESP_OK, "eof without done");
    CHECK_EQ(col.count, 2);
    CHECK(col.events[1].type == OTOOL_LLM_TEXT_EVENT_ERROR, "error");
    CHECK_EQ(col.events[1].data.error.code, OTOOL_LLM_ERR_PROTOCOL_EOF);
    free_collector(&col);
    free(ctx);
}

static void test_chat_invalid_json(void)
{
    otool_llm_protocol_ops_t *ops = (otool_llm_protocol_ops_t *)&otool_llm_protocol_chat;
    otool_llm_exec_ctx_t *ctx = make_ctx();
    collector_t col = { 0 };
    ctx->user_ctx = &col;
    ctx->emit = collector_emit;
    CHECK(feed_adapter(ops, ctx, "message", "{bad") != ESP_OK, "bad json");
    CHECK_EQ(col.count, 1);
    CHECK(col.events[0].type == OTOOL_LLM_TEXT_EVENT_ERROR, "error");
    CHECK_EQ(col.events[0].data.error.code, OTOOL_LLM_ERR_JSON);
    free_collector(&col);
    free(ctx);
}

/* ---- WP5: chat tool calling tests ---- */

static void test_chat_tool_call_flow(void)
{
    otool_llm_protocol_ops_t *ops = (otool_llm_protocol_ops_t *)&otool_llm_protocol_chat;
    otool_llm_exec_ctx_t *ctx = make_ctx();
    collector_t col = { 0 };
    ctx->user_ctx = &col;
    ctx->emit = collector_emit;

    /* 首个 chunk：工具 id + name */
    CHECK(feed_adapter(ops, ctx, "message",
                       "{\"id\":\"cmpl-1\",\"choices\":[{\"index\":0,"
                       "\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_9\",\"type\":\"function\","
                       "\"function\":{\"name\":\"get_weather\",\"arguments\":\"\"}}]},"
                       "\"finish_reason\":null}]}") == ESP_OK,
          "tool start");
    /* started + 空 arguments delta = 2（空 delta 也允许，与 Responses 一致） */
    CHECK_EQ(col.count, 2);
    CHECK(col.events[0].type == OTOOL_LLM_TEXT_EVENT_TOOL_CALL_STARTED, "tool started");
    CHECK_STR(col.events[0].data.tool_call_started.call_id, "call_9");
    CHECK_STR(col.events[0].data.tool_call_started.name, "get_weather");
    CHECK(col.events[1].type == OTOOL_LLM_TEXT_EVENT_TOOL_ARGUMENTS_DELTA, "empty args delta");
    CHECK_EQ(col.events[1].data.tool_arguments_delta.delta_len, 0);

    /* 参数分片 */
    CHECK(feed_adapter(ops, ctx, "message",
                       "{\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,"
                       "\"function\":{\"arguments\":\"{\\\"city\\\":\\\"\"}}]},"
                       "\"finish_reason\":null}]}") == ESP_OK,
          "args 1");
    CHECK(feed_adapter(ops, ctx, "message",
                       "{\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,"
                       "\"function\":{\"arguments\":\"北京\\\"}\"}}]},"
                       "\"finish_reason\":null}]}") == ESP_OK,
          "args 2");
    CHECK_EQ(col.count, 4);
    CHECK(col.events[2].type == OTOOL_LLM_TEXT_EVENT_TOOL_ARGUMENTS_DELTA, "args delta");
    CHECK(col.events[3].type == OTOOL_LLM_TEXT_EVENT_TOOL_ARGUMENTS_DELTA, "args delta 2");

    /* finish_reason=tool_calls + [DONE] → 补发 TOOL_CALL_DONE + COMPLETED */
    CHECK(feed_adapter(ops, ctx, "message",
                       "{\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]}") ==
              ESP_OK,
          "finish tool_calls");
    CHECK(feed_adapter(ops, ctx, "message", "[DONE]") == ESP_OK, "done");
    CHECK_EQ(col.count, 6);
    CHECK(col.events[4].type == OTOOL_LLM_TEXT_EVENT_TOOL_CALL_DONE, "tool done at [DONE]");
    CHECK_STR(col.events[4].data.tool_call_done.arguments, "{\"city\":\"北京\"}");
    CHECK_STR(col.events[4].data.tool_call_done.name, "get_weather");
    CHECK(col.events[5].type == OTOOL_LLM_TEXT_EVENT_COMPLETED, "completed");
    CHECK(ctx->terminal_sent, "terminal");

    free_collector(&col);
    free(ctx);
}

static void test_chat_build_request_with_tools(void)
{
    otool_llm_protocol_ops_t *ops = (otool_llm_protocol_ops_t *)&otool_llm_protocol_chat;
    otool_llm_request_view_t view = make_view();
    static const otool_llm_tool_definition_t tools[] = {
        {
            .struct_size = sizeof(otool_llm_tool_definition_t),
            .name = "get_weather",
            .description = "Get weather",
            .parameters_json_schema =
                "{\"type\":\"object\",\"properties\":{\"city\":{\"type\":\"string\"}},"
                "\"required\":[\"city\"],\"additionalProperties\":false}",
            .strict = true,
        },
    };
    /* assistant tool_calls 消息 + tool 结果消息（LOCAL_TRANSCRIPT 结构） */
    static const otool_llm_tool_call_msg_t calls[] = {
        { .id = "call_9", .name = "get_weather", .arguments = "{\"city\":\"北京\"}" },
    };
    static otool_llm_text_message_t msgs[] = {
        { .role = OTOOL_LLM_ROLE_USER, .text = "天气?" },
        { .role = OTOOL_LLM_ROLE_ASSISTANT, .text = "", .tool_calls = calls,
          .tool_call_count = 1 },
        { .role = OTOOL_LLM_ROLE_TOOL, .text = "{\"ok\":true}", .tool_call_id = "call_9" },
    };
    view.tools = tools;
    view.tool_count = 1;
    view.messages = msgs;
    view.message_count = 3;

    char buf[8192];
    size_t len = 0;
    const otool_llm_provider_preset_t *provider = otool_llm_provider_get(OTOOL_LLM_PROVIDER_OPENAI);
    CHECK(ops->build_request(&view, provider, buf, sizeof(buf), &len) == ESP_OK, "build chat tools");
    CHECK(strstr(buf, "\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"get_weather\"") != NULL,
          "chat tool def");
    CHECK(strstr(buf, "\"tool_calls\":[{\"id\":\"call_9\",\"type\":\"function\"") != NULL,
          "assistant tool_calls");
    CHECK(strstr(buf, "\"role\":\"tool\",\"tool_call_id\":\"call_9\"") != NULL, "tool role msg");
}

/* ---- provider error parser tests ---- */

static void test_provider_error_parsers(void)
{
    char msg[192], code[64], rid[128];

    const otool_llm_provider_preset_t *openai = otool_llm_provider_get(OTOOL_LLM_PROVIDER_OPENAI);
    const char *openai_body =
        "{\"error\":{\"message\":\"You exceeded your current quota\",\"type\":\"insufficient_quota\","
        "\"param\":null,\"code\":\"insufficient_quota\"}}";
    openai->parse_provider_error(openai_body, strlen(openai_body), msg, sizeof(msg), code, sizeof(code),
                                 rid, sizeof(rid));
    CHECK_STR(msg, "You exceeded your current quota");
    CHECK_STR(code, "insufficient_quota");
    CHECK_STR(rid, "");

    const otool_llm_provider_preset_t *ark = otool_llm_provider_get(OTOOL_LLM_PROVIDER_VOLCENGINE_ARK);
    const char *ark_body = "{\"error\":{\"code\":\"InvalidParameter\",\"message\":\"bad model\","
                           "\"request_id\":\"20260822120000A1B2C3\"}}";
    ark->parse_provider_error(ark_body, strlen(ark_body), msg, sizeof(msg), code, sizeof(code),
                              rid, sizeof(rid));
    CHECK_STR(msg, "bad model");
    CHECK_STR(code, "InvalidParameter");
    CHECK_STR(rid, "20260822120000A1B2C3");

    /* bare shape */
    const char *bare = "{\"code\":\"Bare\",\"message\":\"bare msg\"}";
    ark->parse_provider_error(bare, strlen(bare), msg, sizeof(msg), code, sizeof(code), rid, sizeof(rid));
    CHECK_STR(msg, "bare msg");
    CHECK_STR(code, "Bare");

    /* non-JSON body falls back to raw prefix */
    ark->parse_provider_error("<html>gateway timeout</html>", 27, msg, sizeof(msg), code, sizeof(code),
                              rid, sizeof(rid));
    CHECK(strstr(msg, "<html>") != NULL, "raw prefix kept");
}

static void test_provider_auth_headers(void)
{
    char buf[128];
    const otool_llm_provider_preset_t *openai = otool_llm_provider_get(OTOOL_LLM_PROVIDER_OPENAI);
    openai->build_auth_header("sk-test-123", buf, sizeof(buf));
    CHECK_STR(buf, "Bearer sk-test-123");
    const otool_llm_provider_preset_t *ark = otool_llm_provider_get(OTOOL_LLM_PROVIDER_VOLCENGINE_ARK);
    ark->build_auth_header("ark-key", buf, sizeof(buf));
    CHECK_STR(buf, "Bearer ark-key");
}

/* ---- protocol resolve tests ---- */

static void test_protocol_resolve(void)
{
    const otool_llm_provider_preset_t *provider = NULL;
    const otool_llm_protocol_ops_t *ops = NULL;
    CHECK(otool_llm_protocol_resolve(OTOOL_LLM_PROVIDER_OPENAI, OTOOL_LLM_PROTOCOL_AUTO,
                                     &provider, &ops) == ESP_OK,
          "openai auto");
    CHECK(ops->id == OTOOL_LLM_PROTOCOL_RESPONSES_SSE, "openai auto -> responses");
    CHECK(otool_llm_protocol_resolve(OTOOL_LLM_PROVIDER_VOLCENGINE_ARK, OTOOL_LLM_PROTOCOL_AUTO,
                                     NULL, &ops) == ESP_OK,
          "ark auto");
    CHECK(ops->id == OTOOL_LLM_PROTOCOL_RESPONSES_SSE, "ark auto -> responses");
    CHECK(otool_llm_protocol_resolve(OTOOL_LLM_PROVIDER_VOLCENGINE_ARK,
                                     OTOOL_LLM_PROTOCOL_CHAT_COMPLETIONS_SSE, NULL, &ops) == ESP_OK,
          "ark chat");
    CHECK(otool_llm_protocol_resolve(OTOOL_LLM_PROVIDER_CUSTOM, OTOOL_LLM_PROTOCOL_AUTO,
                                     NULL, &ops) != ESP_OK,
          "custom auto rejected");
    CHECK(otool_llm_protocol_resolve(OTOOL_LLM_PROVIDER_CUSTOM, OTOOL_LLM_PROTOCOL_RESPONSES_SSE,
                                     NULL, &ops) == ESP_OK,
          "custom explicit ok");
}

/* ---- main ---- */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    puts("t:sse_basic"); test_sse_basic();
    puts("t:line_endings"); test_sse_line_endings();
    puts("t:multi_data"); test_sse_multi_data();
    puts("t:comment"); test_sse_comment_unknown_retry();
    puts("t:event_id"); test_sse_event_name_and_id();
    puts("t:empty"); test_sse_empty_data_no_dispatch();
    puts("t:multi_events"); test_sse_multiple_events_one_chunk();
    puts("t:leading_space"); test_sse_value_leading_space();
    puts("t:sharding"); test_sse_sharding();
    puts("t:byte_sharding"); test_sse_byte_sharding();
    puts("t:cap"); test_sse_cap_and_overflow();
    puts("t:eof_half"); test_sse_eof_half_event();
    puts("t:crlf_mixed"); test_sse_crlf_mixed();
    puts("t:crlf_across"); test_sse_crlf_across_feed_boundary();

    puts("t:resp_happy"); test_responses_happy_path();
    puts("t:resp_incomplete"); test_responses_incomplete();
    puts("t:resp_failed"); test_responses_failed_and_error_events();
    puts("t:resp_unknown"); test_responses_unknown_events_tolerated();
    puts("t:resp_done"); test_responses_ark_done_marker();
    puts("t:resp_badjson"); test_responses_bad_json_and_types();
    puts("t:resp_eof"); test_responses_on_eof();
    puts("t:resp_tool_flow"); test_responses_tool_call_flow();
    puts("t:resp_tool_mismatch"); test_responses_tool_arguments_mismatch();
    puts("t:resp_tool_half"); test_responses_tool_half_call_at_terminal();
    puts("t:tool_identity_bounds"); test_tool_identity_and_index_bounds();
    puts("t:resp_build_tools"); test_responses_build_request_with_tools();

    puts("t:chat_happy"); test_chat_happy_path();
    puts("t:chat_multi"); test_chat_multiple_choices_rejected();
    puts("t:chat_error"); test_chat_error_event_and_done_eof();
    puts("t:chat_badjson"); test_chat_invalid_json();
    puts("t:chat_tool_flow"); test_chat_tool_call_flow();
    puts("t:chat_build_tools"); test_chat_build_request_with_tools();

    puts("t:provider_err"); test_provider_error_parsers();
    puts("t:provider_auth"); test_provider_auth_headers();
    puts("t:resolve"); test_protocol_resolve();
    puts("t:reg_basic"); test_registry_basic();
    puts("t:reg_schema"); test_registry_schema_rejection();
    puts("t:args_validate"); test_arguments_validation();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
