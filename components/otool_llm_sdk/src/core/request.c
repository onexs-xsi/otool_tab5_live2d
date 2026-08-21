/*
 * SPDX-FileCopyrightText: 2026 otool project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "otool_llm_internal.h"
#include "otool_llm_protocol.h"
#include "otool_llm_transport.h"

#include "esp_log.h"
#include "sdkconfig.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "otool_llm_request";

#ifndef CONFIG_OTOOL_LLM_MAX_REQUEST_BYTES
#define CONFIG_OTOOL_LLM_MAX_REQUEST_BYTES 32768
#endif

/* ---------------- role helpers ---------------- */

/* role name mapping lives in the protocol adapters; request.c does not need it */

/* ---------------- cancel coordination ---------------- */

/* P0-2: 跨任务取消只关闭当前 socket（esp_http_client_close），
 * 不使用 esp_http_client_cancel_request()——其 IDF 实现会关闭后重新 connect，
 * 可能重发 POST（计划 §7.1 禁止自动重试）。
 * 不持锁执行 close（阻塞操作放锁外）：锁内仅置标志并摘走 handle。 */
static esp_err_t request_cancel_locked(otool_llm_request_handle_t request,
                                       esp_http_client_handle_t *out_http)
{
    /* caller holds request->lock */
    request->cancel_requested = true;
    if (out_http != NULL) {
        *out_http = request->http;
        request->http = NULL; /* 摘走：执行任务清理时不再重复 close */
    }
    return ESP_OK;
}

esp_err_t otool_llm_request_cancel(otool_llm_request_handle_t request)
{
    if (request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(request->lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_http_client_handle_t http = NULL;
    esp_err_t err = ESP_OK;
    if (request->executing) {
        err = request_cancel_locked(request, &http);
    }
    xSemaphoreGive(request->lock);

    if (http != NULL) {
        /* 锁外关闭：仅断开当前连接，不重连、不重发请求 */
        esp_err_t cerr = esp_http_client_close(http);
        if (cerr != ESP_OK) {
            ESP_LOGD(TAG, "esp_http_client_close: %s", esp_err_to_name(cerr));
        }
    }
    return err;
}

/* ---------------- event emitter ---------------- */

static esp_err_t exec_emit(otool_llm_exec_ctx_t *ctx, const otool_llm_text_event_t *evt)
{
    if (ctx->terminal_sent) {
        /* A request produces exactly one terminal event; ignore everything after. */
        return ESP_OK;
    }

    otool_llm_text_event_t copy = *evt;
    copy.response_id = ctx->response_id[0] != '\0' ? ctx->response_id : NULL;
    copy.model = ctx->model[0] != '\0' ? ctx->model : NULL;
    copy.request_id = ctx->request_id[0] != '\0' ? ctx->request_id : NULL;

    switch (copy.type) {
    case OTOOL_LLM_TEXT_EVENT_COMPLETED:
    case OTOOL_LLM_TEXT_EVENT_INCOMPLETE:
    case OTOOL_LLM_TEXT_EVENT_CANCELLED:
    case OTOOL_LLM_TEXT_EVENT_ERROR:
        ctx->terminal_sent = true;
        break;
    default:
        break;
    }

    if (ctx->callback != NULL) {
        otool_llm_event_action_t action = ctx->callback(&copy, ctx->user_ctx);
        if (action == OTOOL_LLM_EVENT_ACTION_CANCEL) {
            ctx->cancel_requested = true;
            otool_llm_request_handle_t req = (otool_llm_request_handle_t)ctx->request_owner;
            esp_http_client_handle_t http = NULL;
            if (req != NULL && xSemaphoreTake(req->lock, portMAX_DELAY) == pdTRUE) {
                request_cancel_locked(req, &http);
                xSemaphoreGive(req->lock);
            }
            if (http != NULL) {
                esp_http_client_close(http); /* 锁外关闭，不重连 */
            }
        }
    }
    return ESP_OK;
}

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
    /* Non-OK lets the transport abort the stream right after the terminal event. */
    return code;
}

/* ---------------- transport hooks ---------------- */

static esp_err_t exec_on_sse_event(void *arg, const char *event_name, const char *data, size_t data_len)
{
    otool_llm_exec_ctx_t *ctx = (otool_llm_exec_ctx_t *)arg;
    return ctx->protocol->on_sse_event(ctx, event_name, data, data_len);
}

static esp_err_t exec_on_error_body(void *arg, const char *body, size_t len)
{
    otool_llm_exec_ctx_t *ctx = (otool_llm_exec_ctx_t *)arg;
    char msg[192] = { 0 };
    char code[64] = { 0 };
    char rid[128] = { 0 };
    ctx->provider->parse_provider_error(body, len, msg, sizeof(msg), code, sizeof(code), rid, sizeof(rid));
    if (rid[0] != '\0') {
        snprintf(ctx->request_id, sizeof(ctx->request_id), "%s", rid);
    }
    char full[256];
    if (ctx->retry_after[0] != '\0') {
        snprintf(full, sizeof(full), "%s (Retry-After: %ss)",
                 msg[0] ? msg : "provider error", ctx->retry_after);
    } else {
        snprintf(full, sizeof(full), "%s", msg[0] ? msg : "provider error");
    }
    otool_llm_exec_report_error(ctx, OTOOL_LLM_ERR_PROVIDER, full,
                                code[0] ? code : NULL);
    return ESP_OK;
}

/* ---------------- create/destroy ---------------- */

esp_err_t otool_llm_request_create(otool_llm_client_handle_t client,
                                   const otool_llm_text_request_t *request,
                                   otool_llm_request_handle_t *out_request)
{
    if (client == NULL || request == NULL || out_request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (request->struct_size < sizeof(otool_llm_text_request_t)) {
        ESP_LOGE(TAG, "text request struct_size %u < %u (older caller?)",
                 (unsigned)request->struct_size, (unsigned)sizeof(otool_llm_text_request_t));
        return ESP_ERR_INVALID_VERSION;
    }
    if (request->model == NULL || request->model[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (request->message_count > 0 && request->messages == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < request->message_count; i++) {
        if (request->messages[i].text == NULL) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    /* Fields the Chat protocol cannot express must be rejected, never silently dropped. */
    if (client->protocol->id == OTOOL_LLM_PROTOCOL_CHAT_COMPLETIONS_SSE) {
        if (request->previous_response_id != NULL) {
            ESP_LOGE(TAG, "previous_response_id is not supported by the Chat protocol");
            return OTOOL_LLM_ERR_UNSUPPORTED;
        }
        if (request->store) {
            ESP_LOGE(TAG, "store is not supported by the Chat protocol");
            return OTOOL_LLM_ERR_UNSUPPORTED;
        }
    }

    otool_llm_request_handle_t r = (otool_llm_request_handle_t)calloc(1, sizeof(*r));
    if (r == NULL) {
        return ESP_ERR_NO_MEM;
    }
    r->client = client;

    esp_err_t err = otool_llm_strdup(request->model, &r->model);
    if (err == ESP_OK) {
        err = otool_llm_strdup(request->instructions, &r->instructions);
    }
    if (err == ESP_OK) {
        err = otool_llm_strdup(request->previous_response_id, &r->previous_response_id);
    }
    if (err == ESP_OK && request->message_count > 0) {
        r->messages = (otool_llm_request_message_t *)calloc(request->message_count, sizeof(*r->messages));
        if (r->messages == NULL) {
            err = ESP_ERR_NO_MEM;
        } else {
            for (size_t i = 0; i < request->message_count; i++) {
                r->messages[i].role = request->messages[i].role;
                err = otool_llm_strdup(request->messages[i].text, &r->messages[i].text);
                if (err != ESP_OK) {
                    break;
                }
            }
            if (err == ESP_OK) {
                r->message_count = request->message_count;
            }
        }
    }
    if (err != ESP_OK) {
        for (size_t i = 0; i < r->message_count; i++) {
            free((void *)r->messages[i].text);
        }
        free((void *)r->messages);
        free((void *)r->previous_response_id);
        free((void *)r->instructions);
        free((void *)r->model);
        free(r);
        return err;
    }

    r->max_output_tokens = request->max_output_tokens;
    r->temperature = request->temperature;
    r->temperature_is_set = request->temperature_is_set;
    r->store = request->store;

    r->lock = xSemaphoreCreateMutex();
    if (r->lock == NULL) {
        for (size_t i = 0; i < r->message_count; i++) {
            free((void *)r->messages[i].text);
        }
        free((void *)r->messages);
        free((void *)r->previous_response_id);
        free((void *)r->instructions);
        free((void *)r->model);
        free(r);
        return ESP_ERR_NO_MEM;
    }

    /* 生命周期保护：client 存活期间 request handle 计数（P1） */
    if (xSemaphoreTake(client->lock, portMAX_DELAY) == pdTRUE) {
        client->request_count++;
        xSemaphoreGive(client->lock);
    }

    *out_request = r;
    return ESP_OK;
}

void otool_llm_request_destroy(otool_llm_request_handle_t request)
{
    if (request == NULL) {
        return;
    }
    if (xSemaphoreTake(request->lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (request->executing) {
        ESP_LOGE(TAG, "refusing to destroy request while execute_stream() is running");
        xSemaphoreGive(request->lock);
        return;
    }
    xSemaphoreGive(request->lock);

    /* 生命周期保护：释放 client 上的 request 计数（P1） */
    if (xSemaphoreTake(request->client->lock, portMAX_DELAY) == pdTRUE) {
        if (request->client->request_count > 0) {
            request->client->request_count--;
        }
        xSemaphoreGive(request->client->lock);
    }

    for (size_t i = 0; i < request->message_count; i++) {
        free((void *)request->messages[i].text);
    }
    free((void *)request->messages);
    free((void *)request->previous_response_id);
    free((void *)request->instructions);
    free((void *)request->model);
    vSemaphoreDelete(request->lock);
    free(request);
}

/* ---------------- execute ---------------- */

esp_err_t otool_llm_request_execute_stream(otool_llm_request_handle_t request,
                                           otool_llm_text_event_cb_t callback,
                                           void *user_ctx)
{
    if (request == NULL || callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    otool_llm_client_handle_t client = request->client;

    /* Claim the request (one execution at a time). */
    if (xSemaphoreTake(request->lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (request->executing) {
        xSemaphoreGive(request->lock);
        return ESP_ERR_INVALID_STATE;
    }
    request->executing = true;
    request->cancel_requested = false;
    request->http = NULL;
    xSemaphoreGive(request->lock);

    /* Claim the client (one in-flight request per client). */
    if (xSemaphoreTake(client->lock, portMAX_DELAY) != pdTRUE) {
        request->executing = false;
        return ESP_ERR_INVALID_STATE;
    }
    if (client->active != NULL) {
        xSemaphoreGive(client->lock);
        request->executing = false;
        return ESP_ERR_INVALID_STATE;
    }
    client->active = request;
    xSemaphoreGive(client->lock);

    /* Prepare the execution context. */
    otool_llm_request_view_t view = {
        .model = request->model,
        .instructions = request->instructions,
        .messages = request->messages,
        .message_count = request->message_count,
        .previous_response_id = request->previous_response_id,
        .max_output_tokens = request->max_output_tokens,
        .temperature = request->temperature,
        .temperature_is_set = request->temperature_is_set,
        .store = request->store,
    };
    otool_llm_exec_ctx_t *ctx = &request->exec;
    memset(ctx, 0, sizeof(*ctx));
    ctx->request = &view;
    ctx->request_owner = request;
    ctx->protocol = client->protocol;
    ctx->provider = client->provider;
    ctx->callback = callback;
    ctx->user_ctx = user_ctx;
    ctx->emit = exec_emit;

    esp_err_t err = ESP_OK;
    char *url = NULL; /* 声明于顶部：out_error_local 路径可能跳过分配点 */

    /* 1. Serialize the request body into a bounded buffer. */
    size_t body_cap = CONFIG_OTOOL_LLM_MAX_REQUEST_BYTES;
    char *body = (char *)malloc(body_cap);
    if (body == NULL) {
        err = ESP_ERR_NO_MEM;
        goto out_error_local;
    }
    size_t body_len = 0;
    err = client->protocol->build_request(&view, client->provider, body, body_cap, &body_len);
    if (err != ESP_OK) {
        goto out_error_local;
    }

    /* 2. Build the URL and the Authorization header. */
    const char *path = NULL;
    if (client->protocol->id == OTOOL_LLM_PROTOCOL_RESPONSES_SSE) {
        path = client->responses_path != NULL ? client->responses_path : client->provider->default_responses_path;
    } else {
        path = client->chat_path != NULL ? client->chat_path : client->provider->default_chat_path;
    }
    if (path == NULL) {
        err = ESP_ERR_INVALID_ARG;
        goto out_error_local;
    }
    url = (char *)malloc(strlen(client->base_url) + strlen(path) + 1);
    if (url == NULL) {
        err = ESP_ERR_NO_MEM;
        goto out_error_local;
    }
    strcpy(url, client->base_url);
    strcat(url, path);

    char auth[1024];
    err = client->provider->build_auth_header(client->api_key, auth, sizeof(auth));
    if (err != ESP_OK) {
        goto out_error_local;
    }

    /* 3. Transport. */
#ifdef CONFIG_OTOOL_LLM_ALLOW_INSECURE_HTTP
    const bool allow_insecure_http = true;
#else
    const bool allow_insecure_http = false;
#endif
    otool_llm_transport_config_t tcfg = {
        .url = url,
        .body = body,
        .body_len = body_len,
        .authorization = auth,
        .connect_timeout_ms = client->connect_timeout_ms,
        .read_timeout_ms = client->read_timeout_ms,
        .allow_insecure_http = allow_insecure_http,
        .request_id_header_name = client->provider->request_id_header_name,
        .request_id_out = ctx->request_id,
        .request_id_size = sizeof(ctx->request_id),
        .retry_after_out = ctx->retry_after,
        .retry_after_size = sizeof(ctx->retry_after),
        .on_sse_event = exec_on_sse_event,
        .sse_arg = ctx,
        .on_error_body = exec_on_error_body,
        .error_arg = ctx,
        .cancel_lock = request->lock,
        .cancel_requested = &request->cancel_requested,
        .active_http = &request->http,
    };
    err = otool_llm_transport_execute(&tcfg);

    /* 4. Guarantee exactly one terminal event. */
    if (!ctx->terminal_sent) {
        if (ctx->cancel_requested) {
            otool_llm_text_event_t evt = { .type = OTOOL_LLM_TEXT_EVENT_CANCELLED };
            ctx->emit(ctx, &evt);
            err = ESP_OK;
        } else if (err == ESP_OK) {
            /* Stream ended cleanly but no terminal event: protocol EOF. */
            client->protocol->on_eof(ctx);
            err = ctx->error_code != 0 ? ctx->error_code : OTOOL_LLM_ERR_PROTOCOL_EOF;
        } else {
            otool_llm_exec_report_error(ctx, err, esp_err_to_name(err), NULL);
            err = ctx->error_code;
        }
    } else if (ctx->error_code != 0) {
        /* Terminal was an ERROR: the return value must carry the real error code
         * (plan §12: terminal ERROR code == API return value). */
        err = ctx->error_code;
    } else {
        err = ESP_OK;
    }

    free(url);
    free(body);
    goto out;

out_error_local:
    /* Local failure (OOM, request too large, bad URL, auth build): emit a terminal ERROR. */
    if (!ctx->terminal_sent) {
        otool_llm_exec_report_error(ctx, err, esp_err_to_name(err), NULL);
    }
    free(url);
    free(body);
    err = ctx->error_code != 0 ? ctx->error_code : err;

out:
    if (xSemaphoreTake(client->lock, portMAX_DELAY) == pdTRUE) {
        client->active = NULL;
        xSemaphoreGive(client->lock);
    }
    if (xSemaphoreTake(request->lock, portMAX_DELAY) == pdTRUE) {
        request->executing = false;
        request->http = NULL;
        xSemaphoreGive(request->lock);
    }
    return err;
}
