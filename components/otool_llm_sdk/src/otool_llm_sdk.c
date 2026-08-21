/*
 * SPDX-FileCopyrightText: 2026 otool project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "otool_llm_sdk.h"

#include "esp_http_client.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CONFIG_OTOOL_LLM_ENDPOINT_URL
#define CONFIG_OTOOL_LLM_ENDPOINT_URL "https://ark.cn-beijing.volces.com/api/v3/chat/completions"
#endif

#ifndef CONFIG_OTOOL_LLM_RECV_TIMEOUT_MS
#define CONFIG_OTOOL_LLM_RECV_TIMEOUT_MS 60000
#endif

typedef struct {
    const otool_llm_chat_request_t *req;
    char *line_buf;
    size_t line_len;
    size_t line_cap;
    bool done;
    bool error_reported;
} otool_llm_stream_ctx_t;

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} otool_json_builder_t;

static void stream_send_event(otool_llm_stream_ctx_t *ctx, otool_llm_event_type_t type,
                              const char *data, size_t data_len)
{
    if (!ctx->req->on_event) {
        return;
    }

    otool_llm_event_t evt = {
        .type = type,
        .data = data,
        .data_len = data_len,
    };
    ctx->req->on_event(&evt, ctx->req->user_ctx);
}

static void stream_report_error(otool_llm_stream_ctx_t *ctx, const char *message)
{
    if (ctx->error_reported) {
        return;
    }
    ctx->error_reported = true;
    stream_send_event(ctx, OTOOL_LLM_EVENT_ERROR, message, strlen(message));
}

static bool json_buf_reserve(otool_json_builder_t *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap) {
        return true;
    }

    size_t new_cap = b->cap ? b->cap * 2 : 256;
    while (new_cap < b->len + extra + 1) {
        new_cap *= 2;
    }

    char *new_buf = (char *)realloc(b->buf, new_cap);
    if (new_buf == NULL) {
        return false;
    }

    b->buf = new_buf;
    b->cap = new_cap;
    return true;
}

static bool json_buf_append(otool_json_builder_t *b, const char *s)
{
    size_t len = strlen(s);
    if (!json_buf_reserve(b, len)) {
        return false;
    }

    memcpy(b->buf + b->len, s, len);
    b->len += len;
    b->buf[b->len] = '\0';
    return true;
}

static bool json_buf_append_escaped_string(otool_json_builder_t *b, const char *s)
{
    static const char hex[] = "0123456789abcdef";

    if (!json_buf_reserve(b, strlen(s) * 6 + 3)) {
        return false;
    }

    b->buf[b->len++] = '"';
    for (const char *p = s; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
        case '"':
            b->buf[b->len++] = '\\';
            b->buf[b->len++] = '"';
            break;
        case '\\':
            b->buf[b->len++] = '\\';
            b->buf[b->len++] = '\\';
            break;
        case '\n':
            b->buf[b->len++] = '\\';
            b->buf[b->len++] = 'n';
            break;
        case '\r':
            b->buf[b->len++] = '\\';
            b->buf[b->len++] = 'r';
            break;
        case '\t':
            b->buf[b->len++] = '\\';
            b->buf[b->len++] = 't';
            break;
        default:
            if (c < 0x20) {
                b->buf[b->len++] = '\\';
                b->buf[b->len++] = 'u';
                b->buf[b->len++] = '0';
                b->buf[b->len++] = '0';
                b->buf[b->len++] = hex[(c >> 4) & 0x0f];
                b->buf[b->len++] = hex[c & 0x0f];
            } else {
                b->buf[b->len++] = (char)c;
            }
            break;
        }
    }
    b->buf[b->len++] = '"';
    b->buf[b->len] = '\0';
    return true;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return 0;
}

static bool append_utf8_char(char *out, size_t *len, size_t cap, unsigned int cp)
{
    if (*len + 4 >= cap) {
        return false;
    }

    if (cp < 0x80) {
        out[(*len)++] = (char)cp;
    } else if (cp < 0x800) {
        out[(*len)++] = (char)(0xc0 | (cp >> 6));
        out[(*len)++] = (char)(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        out[(*len)++] = (char)(0xe0 | (cp >> 12));
        out[(*len)++] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[(*len)++] = (char)(0x80 | (cp & 0x3f));
    } else {
        out[(*len)++] = (char)(0xf0 | (cp >> 18));
        out[(*len)++] = (char)(0x80 | ((cp >> 12) & 0x3f));
        out[(*len)++] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[(*len)++] = (char)(0x80 | (cp & 0x3f));
    }
    return true;
}

static bool extract_json_string_value(const char *json, const char *key,
                                      char *out, size_t out_size)
{
    char key_pattern[64];
    snprintf(key_pattern, sizeof(key_pattern), "\"%s\"", key);

    const char *p = strstr(json, key_pattern);
    if (p == NULL) {
        return false;
    }
    p += strlen(key_pattern);

    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != ':') {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;

    size_t olen = 0;
    while (*p != '\0' && olen + 1 < out_size) {
        char c = *p++;
        if (c == '"') {
            out[olen] = '\0';
            return olen > 0;
        }

        if (c == '\\' && *p != '\0') {
            char e = *p++;
            switch (e) {
            case 'n':
                out[olen++] = '\n';
                break;
            case 'r':
                out[olen++] = '\r';
                break;
            case 't':
                out[olen++] = '\t';
                break;
            case 'b':
                out[olen++] = '\b';
                break;
            case 'f':
                out[olen++] = '\f';
                break;
            case '"':
                out[olen++] = '"';
                break;
            case '\\':
                out[olen++] = '\\';
                break;
            case '/':
                out[olen++] = '/';
                break;
            case 'u': {
                if (isxdigit((unsigned char)p[0]) && isxdigit((unsigned char)p[1]) &&
                    isxdigit((unsigned char)p[2]) && isxdigit((unsigned char)p[3])) {
                    unsigned int cp = 0;
                    for (int i = 0; i < 4; i++) {
                        cp = (cp << 4) | (unsigned int)hex_value(p[i]);
                    }
                    p += 4;
                    if (!append_utf8_char(out, &olen, out_size, cp)) {
                        return false;
                    }
                } else {
                    out[olen++] = '?';
                }
                break;
            }
            default:
                out[olen++] = e;
                break;
            }
        } else {
            out[olen++] = c;
        }
    }

    out[olen] = '\0';
    return olen > 0;
}

static bool line_buf_append_char(otool_llm_stream_ctx_t *ctx, char c)
{
    if (ctx->line_len + 2 > ctx->line_cap) {
        size_t new_cap = ctx->line_cap ? ctx->line_cap * 2 : 256;
        char *new_buf = (char *)realloc(ctx->line_buf, new_cap);
        if (new_buf == NULL) {
            return false;
        }
        ctx->line_buf = new_buf;
        ctx->line_cap = new_cap;
    }

    ctx->line_buf[ctx->line_len++] = c;
    ctx->line_buf[ctx->line_len] = '\0';
    return true;
}

static void stream_process_line(otool_llm_stream_ctx_t *ctx)
{
    if (ctx->line_len == 0) {
        return;
    }

    char *line = ctx->line_buf;
    if (strncmp(line, "data:", 5) != 0) {
        ctx->line_len = 0;
        return;
    }

    char *payload = line + 5;
    while (*payload == ' ' || *payload == '\t') {
        payload++;
    }

    if (strcmp(payload, "[DONE]") == 0) {
        if (!ctx->done) {
            ctx->done = true;
            stream_send_event(ctx, OTOOL_LLM_EVENT_DONE, NULL, 0);
        }
        ctx->line_len = 0;
        return;
    }

    char content[1024];
    if (extract_json_string_value(payload, "content", content, sizeof(content))) {
        stream_send_event(ctx, OTOOL_LLM_EVENT_DELTA, content, strlen(content));
    }

    ctx->line_len = 0;
}

static void stream_feed(otool_llm_stream_ctx_t *ctx, const char *data, int data_len)
{
    for (int i = 0; i < data_len; i++) {
        char c = data[i];
        if (c == '\n') {
            stream_process_line(ctx);
            ctx->line_len = 0;
        } else if (c != '\r') {
            if (!line_buf_append_char(ctx, c)) {
                stream_report_error(ctx, "line buffer OOM");
                return;
            }
        }
    }
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    otool_llm_stream_ctx_t *ctx = (otool_llm_stream_ctx_t *)evt->user_data;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_CONNECTED:
        stream_send_event(ctx, OTOOL_LLM_EVENT_CONNECTED, NULL, 0);
        break;

    case HTTP_EVENT_ON_DATA:
        if (evt->status_code >= 400) {
            char msg[64];
            snprintf(msg, sizeof(msg), "HTTP error %d", evt->status_code);
            stream_report_error(ctx, msg);
        } else if (evt->data != NULL && evt->data_len > 0) {
            stream_feed(ctx, (const char *)evt->data, evt->data_len);
        }
        break;

    case HTTP_EVENT_ON_FINISH:
        if (evt->status_code >= 400 && !ctx->error_reported) {
            char msg[64];
            snprintf(msg, sizeof(msg), "HTTP error %d", evt->status_code);
            stream_report_error(ctx, msg);
        }
        if (ctx->line_len > 0) {
            stream_process_line(ctx);
        }
        if (!ctx->done && !ctx->error_reported) {
            ctx->done = true;
            stream_send_event(ctx, OTOOL_LLM_EVENT_DONE, NULL, 0);
        }
        ctx->done = true;
        break;

    case HTTP_EVENT_DISCONNECTED:
        /* Normal completion is handled in HTTP_EVENT_ON_FINISH.
         * If the connection drops unexpectedly, esp_http_client_perform()
         * returns an error and otool_llm_chat_stream() reports it. */
        break;

    default:
        break;
    }

    return ESP_OK;
}

esp_err_t otool_llm_chat_stream(const otool_llm_chat_request_t *req)
{
    if (req == NULL || req->api_key == NULL || req->model == NULL ||
        req->user_message == NULL || req->on_event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    otool_json_builder_t body = { .buf = NULL, .len = 0, .cap = 0 };
    if (!json_buf_append(&body, "{\"model\":")) {
        free(body.buf);
        return ESP_ERR_NO_MEM;
    }
    if (!json_buf_append_escaped_string(&body, req->model)) {
        free(body.buf);
        return ESP_ERR_NO_MEM;
    }
    if (!json_buf_append(&body, ",\"stream\":true")) {
        free(body.buf);
        return ESP_ERR_NO_MEM;
    }

    if (req->max_tokens > 0) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), ",\"max_tokens\":%d", req->max_tokens);
        if (!json_buf_append(&body, tmp)) {
            free(body.buf);
            return ESP_ERR_NO_MEM;
        }
    }

    if (req->temperature >= 0.0f) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), ",\"temperature\":%.2f", (double)req->temperature);
        if (!json_buf_append(&body, tmp)) {
            free(body.buf);
            return ESP_ERR_NO_MEM;
        }
    }

    if (!json_buf_append(&body, ",\"messages\":[")) {
        free(body.buf);
        return ESP_ERR_NO_MEM;
    }

    if (req->system_prompt != NULL && req->system_prompt[0] != '\0') {
        if (!json_buf_append(&body, "{\"role\":\"system\",\"content\":")) {
            free(body.buf);
            return ESP_ERR_NO_MEM;
        }
        if (!json_buf_append_escaped_string(&body, req->system_prompt)) {
            free(body.buf);
            return ESP_ERR_NO_MEM;
        }
        if (!json_buf_append(&body, "},")) {
            free(body.buf);
            return ESP_ERR_NO_MEM;
        }
    }

    if (!json_buf_append(&body, "{\"role\":\"user\",\"content\":")) {
        free(body.buf);
        return ESP_ERR_NO_MEM;
    }
    if (!json_buf_append_escaped_string(&body, req->user_message)) {
        free(body.buf);
        return ESP_ERR_NO_MEM;
    }
    if (!json_buf_append(&body, "}]}")) {
        free(body.buf);
        return ESP_ERR_NO_MEM;
    }

    otool_llm_stream_ctx_t ctx = {
        .req = req,
        .line_buf = NULL,
        .line_len = 0,
        .line_cap = 0,
        .done = false,
        .error_reported = false,
    };

    esp_http_client_config_t config = {
        .url = CONFIG_OTOOL_LLM_ENDPOINT_URL,
        .event_handler = http_event_handler,
        .user_data = &ctx,
        .timeout_ms = req->timeout_ms > 0 ? req->timeout_ms : CONFIG_OTOOL_LLM_RECV_TIMEOUT_MS,
        .buffer_size = 4096,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(body.buf);
        free(ctx.line_buf);
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "text/event-stream");

    char auth[512];
    snprintf(auth, sizeof(auth), "Bearer %s", req->api_key);
    esp_http_client_set_header(client, "Authorization", auth);

    esp_http_client_set_post_field(client, body.buf, (int)strlen(body.buf));

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK && !ctx.error_reported) {
        stream_report_error(&ctx, esp_err_to_name(err));
    }
    if (err == ESP_OK && ctx.error_reported) {
        err = ESP_FAIL;
    }

    esp_http_client_cleanup(client);
    free(body.buf);
    free(ctx.line_buf);

    return err;
}