/*
 * SPDX-FileCopyrightText: 2026 otool project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * HTTP SSE stream transport on top of esp_http_client. Knows nothing about
 * Responses/Chat event semantics: it validates status/content-type, feeds the
 * SSE parser and forwards events. Cancel uses esp_http_client_close() (not
 * cancel_request, which reconnects and could re-send the POST — the plan
 * forbids automatic retries).
 */

#include "otool_llm_transport.h"

#include "otool_llm_sse_parser.h"

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "otool_llm_http";

#ifndef CONFIG_OTOOL_LLM_HTTP_RX_BUFFER_BYTES
#define CONFIG_OTOOL_LLM_HTTP_RX_BUFFER_BYTES 2048
#endif
#ifndef CONFIG_OTOOL_LLM_MAX_REQUEST_BYTES
#define CONFIG_OTOOL_LLM_MAX_REQUEST_BYTES 32768
#endif
#ifndef CONFIG_OTOOL_LLM_MAX_ERROR_BODY_BYTES
#define CONFIG_OTOOL_LLM_MAX_ERROR_BODY_BYTES 4096
#endif
#ifndef CONFIG_OTOOL_LLM_MAX_SSE_EVENT_BYTES
#define CONFIG_OTOOL_LLM_MAX_SSE_EVENT_BYTES 16384
#endif

typedef struct {
    const otool_llm_transport_config_t *cfg;
    otool_llm_sse_parser_t *parser;
    bool content_type_ok;
    bool saw_error_body;      /* non-2xx body started */
    bool error_body_too_large;
    char *error_body;
    size_t error_body_len;
    esp_err_t abort_error;    /* set when the SSE sink asked to stop */
    int status_code;
} otool_llm_transport_ctx_t;

static bool header_equals(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static esp_err_t copy_header_value(const char *value, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0 || value == NULL) {
        return ESP_OK;
    }
    snprintf(out, out_size, "%s", value);
    return ESP_OK;
}

static esp_err_t feed_chunk(otool_llm_transport_ctx_t *c, const uint8_t *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        otool_llm_sse_event_t evt;
        size_t consumed = 0;
        otool_llm_sse_feed_result_t r = otool_llm_sse_parser_feed(c->parser,
                                                                   data + off, len - off,
                                                                   &evt, &consumed);
        off += consumed;
        if (r == OTOOL_LLM_SSE_FEED_ERROR) {
            ESP_LOGE(TAG, "SSE event exceeded the configured cap");
            return OTOOL_LLM_ERR_EVENT_TOO_LARGE;
        }
        if (r == OTOOL_LLM_SSE_FEED_EVENT) {
            esp_err_t err = c->cfg->on_sse_event(c->cfg->sse_arg, evt.event, evt.data, evt.data_len);
            if (err != ESP_OK) {
                c->abort_error = err;
                return err;
            }
        }
        if (r == OTOOL_LLM_SSE_FEED_OK) {
            break; /* all remaining bytes consumed without a complete event */
        }
    }
    return ESP_OK;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    otool_llm_transport_ctx_t *c = (otool_llm_transport_ctx_t *)evt->user_data;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_CONNECTED:
        /* Connect phase done: switch to the stream read timeout. */
        esp_http_client_set_timeout_ms(evt->client, c->cfg->read_timeout_ms);
        break;

    case HTTP_EVENT_ON_HEADER:
        if (header_equals(evt->header_key, "Content-Type")) {
            c->content_type_ok = evt->header_value != NULL &&
                                 strncasecmp(evt->header_value, "text/event-stream", 17) == 0;
        }
        if (c->cfg->request_id_header_name != NULL &&
            header_equals(evt->header_key, c->cfg->request_id_header_name)) {
            copy_header_value(evt->header_value, c->cfg->request_id_out, c->cfg->request_id_size);
        }
        if (header_equals(evt->header_key, "Retry-After")) {
            copy_header_value(evt->header_value, c->cfg->retry_after_out, c->cfg->retry_after_size);
        }
        break;

    case HTTP_EVENT_ON_DATA: {
        int status = esp_http_client_get_status_code(evt->client);
        c->status_code = status;
        if (status >= 400) {
            /* Collect a bounded error body; overflow is reported, never truncated silently. */
            if (c->error_body == NULL) {
                c->error_body = (char *)malloc(CONFIG_OTOOL_LLM_MAX_ERROR_BODY_BYTES);
                if (c->error_body == NULL) {
                    return ESP_ERR_NO_MEM;
                }
                c->saw_error_body = true;
            }
            size_t room = CONFIG_OTOOL_LLM_MAX_ERROR_BODY_BYTES - c->error_body_len;
            if (evt->data != NULL && (size_t)evt->data_len >= room) {
                memcpy(c->error_body + c->error_body_len, evt->data, room);
                c->error_body_len += room;
                c->error_body_too_large = true;
            } else if (evt->data != NULL) {
                memcpy(c->error_body + c->error_body_len, evt->data, evt->data_len);
                c->error_body_len += (size_t)evt->data_len;
            }
            return ESP_OK;
        }
        if (evt->data != NULL && evt->data_len > 0) {
            esp_err_t err = feed_chunk(c, (const uint8_t *)evt->data, (size_t)evt->data_len);
            if (err != ESP_OK) {
                /* Stop the stream: the SSE sink asked to abort (e.g. terminal event error). */
                esp_http_client_close(evt->client);
                return ESP_OK;
            }
        }
        break;
    }

    case HTTP_EVENT_ON_FINISH:
    case HTTP_EVENT_DISCONNECTED:
    default:
        break;
    }

    return ESP_OK;
}

esp_err_t otool_llm_transport_execute(const otool_llm_transport_config_t *cfg)
{
    if (cfg == NULL || cfg->url == NULL || cfg->body == NULL ||
        cfg->authorization == NULL || cfg->on_sse_event == NULL ||
        cfg->cancel_lock == NULL || cfg->cancel_requested == NULL ||
        cfg->active_http == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    otool_llm_transport_ctx_t c = {
        .cfg = cfg,
        .abort_error = ESP_OK,
        .status_code = 0,
    };
    c.parser = otool_llm_sse_parser_create(CONFIG_OTOOL_LLM_MAX_SSE_EVENT_BYTES);
    if (c.parser == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t http_cfg = {
        .url = cfg->url,
        .event_handler = http_event_handler,
        .user_data = &c,
        .timeout_ms = cfg->connect_timeout_ms > 0 ? cfg->connect_timeout_ms : CONFIG_OTOOL_LLM_CONNECT_TIMEOUT_MS,
        .buffer_size = CONFIG_OTOOL_LLM_HTTP_RX_BUFFER_BYTES,
        .buffer_size_tx = CONFIG_OTOOL_LLM_MAX_REQUEST_BYTES,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        otool_llm_sse_parser_destroy(c.parser);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "text/event-stream");
    esp_http_client_set_header(client, "Authorization", cfg->authorization);
    esp_http_client_set_post_field(client, cfg->body, (int)cfg->body_len);

    /* Publish the active handle for cross-task cancel, then check for a racing cancel. */
    if (xSemaphoreTake(cfg->cancel_lock, portMAX_DELAY) != pdTRUE) {
        esp_http_client_cleanup(client);
        otool_llm_sse_parser_destroy(c.parser);
        return ESP_ERR_INVALID_STATE;
    }
    *cfg->active_http = client;
    bool cancel_already = *cfg->cancel_requested;
    xSemaphoreGive(cfg->cancel_lock);

    esp_err_t err = ESP_OK;
    if (!cancel_already) {
        err = esp_http_client_perform(client);
    }

    if (xSemaphoreTake(cfg->cancel_lock, portMAX_DELAY) == pdTRUE) {
        *cfg->active_http = NULL;
        bool cancelled = *cfg->cancel_requested;
        xSemaphoreGive(cfg->cancel_lock);

        if (c.abort_error != ESP_OK) {
            err = c.abort_error;
        } else if (cancelled) {
            err = ESP_OK; /* executor turns this into the CANCELLED terminal event */
        } else if (err == ESP_OK) {
            int status = esp_http_client_get_status_code(client);
            if (status < 200 || status >= 300) {
                /* Non-2xx: hand the bounded body to the provider error parser. */
                if (c.saw_error_body && c.error_body != NULL) {
                    if (c.error_body_too_large) {
                        ESP_LOGW(TAG, "error body exceeded the configured cap");
                    }
                    cfg->on_error_body(cfg->error_arg, c.error_body, c.error_body_len);
                }
                err = OTOOL_LLM_ERR_HTTP_STATUS;
            } else if (!c.content_type_ok) {
                err = OTOOL_LLM_ERR_BAD_CONTENT_TYPE;
            } else if (otool_llm_sse_parser_finish(c.parser) != ESP_OK) {
                /* Stream ended with a half event: protocol EOF, never a success. */
                err = OTOOL_LLM_ERR_PROTOCOL_EOF;
            }
        }
    } else {
        err = ESP_ERR_INVALID_STATE;
    }

    esp_http_client_cleanup(client);
    free(c.error_body);
    otool_llm_sse_parser_destroy(c.parser);
    return err;
}
