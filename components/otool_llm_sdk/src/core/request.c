/*
 * SPDX-FileCopyrightText: 2026 otool project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "otool_llm_internal.h"
#include "otool_llm_protocol.h"
#include "otool_llm_tool_schema.h"
#include "otool_llm_transport.h"

#include "esp_log.h"
#include "sdkconfig.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "otool_llm_request";

#ifndef CONFIG_OTOOL_LLM_MAX_REQUEST_BYTES
#define CONFIG_OTOOL_LLM_MAX_REQUEST_BYTES 32768
#endif
#ifndef CONFIG_OTOOL_LLM_MAX_TOOLS
#define CONFIG_OTOOL_LLM_MAX_TOOLS 8
#endif
#ifndef CONFIG_OTOOL_LLM_MAX_REQUEST_MESSAGES
#define CONFIG_OTOOL_LLM_MAX_REQUEST_MESSAGES 32
#endif
#ifndef CONFIG_OTOOL_LLM_MAX_TOOL_NAME_BYTES
#define CONFIG_OTOOL_LLM_MAX_TOOL_NAME_BYTES 64
#endif
#ifndef CONFIG_OTOOL_LLM_MAX_TOOL_DESCRIPTION_BYTES
#define CONFIG_OTOOL_LLM_MAX_TOOL_DESCRIPTION_BYTES 512
#endif
#ifndef CONFIG_OTOOL_LLM_MAX_TOOL_ARGUMENT_BYTES
#define CONFIG_OTOOL_LLM_MAX_TOOL_ARGUMENT_BYTES 4096
#endif
#ifndef CONFIG_OTOOL_LLM_MAX_TOOL_OUTPUT_BYTES
#define CONFIG_OTOOL_LLM_MAX_TOOL_OUTPUT_BYTES 4096
#endif
#ifndef CONFIG_OTOOL_LLM_MAX_TOOL_SCHEMA_BYTES
#define CONFIG_OTOOL_LLM_MAX_TOOL_SCHEMA_BYTES 2048
#endif
#ifndef CONFIG_OTOOL_LLM_MAX_TOTAL_TOOL_SCHEMA_BYTES
#define CONFIG_OTOOL_LLM_MAX_TOTAL_TOOL_SCHEMA_BYTES 8192
#endif

#define OTOOL_LLM_MAX_MODEL_BYTES 256
#define OTOOL_LLM_MAX_PROVIDER_ID_BYTES 127
#define OTOOL_LLM_MAX_TOOL_CALL_ID_BYTES 63

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

static void request_messages_free(otool_llm_request_message_t *messages, size_t count)
{
    if (messages == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        if (messages[i].text != NULL) {
            otool_llm_secure_zero((void *)messages[i].text, strlen(messages[i].text));
        }
        free((void *)messages[i].text);
        free((void *)messages[i].tool_call_id);
        for (size_t j = 0; j < messages[i].tool_call_count; j++) {
            if (messages[i].tool_calls[j].arguments != NULL) {
                otool_llm_secure_zero((void *)messages[i].tool_calls[j].arguments,
                                      strlen(messages[i].tool_calls[j].arguments));
            }
            free((void *)messages[i].tool_calls[j].arguments);
            free((void *)messages[i].tool_calls[j].name);
            free((void *)messages[i].tool_calls[j].id);
        }
        free((void *)messages[i].tool_calls);
    }
    free((void *)messages);
}

static void request_data_free(otool_llm_request_handle_t request)
{
    if (request == NULL) {
        return;
    }
    for (size_t i = 0; i < request->tool_output_count; i++) {
        if (request->tool_outputs[i].output != NULL) {
            otool_llm_secure_zero((void *)request->tool_outputs[i].output,
                                  strlen(request->tool_outputs[i].output));
        }
        free((void *)request->tool_outputs[i].output);
        free((void *)request->tool_outputs[i].call_id);
    }
    free((void *)request->tool_outputs);
    for (size_t i = 0; i < request->tool_count; i++) {
        free((void *)request->tools[i].parameters_json_schema);
        free((void *)request->tools[i].description);
        free((void *)request->tools[i].name);
    }
    free((void *)request->tools);
    request_messages_free(request->messages, request->message_count);
    if (request->previous_response_id != NULL) {
        otool_llm_secure_zero((void *)request->previous_response_id,
                              strlen(request->previous_response_id));
    }
    free((void *)request->previous_response_id);
    if (request->instructions != NULL) {
        otool_llm_secure_zero((void *)request->instructions, strlen(request->instructions));
    }
    free((void *)request->instructions);
    free((void *)request->model);
}

static bool bounded_string_length(const char *value, size_t maximum, size_t *out_length)
{
    if (value == NULL) {
        if (out_length != NULL) {
            *out_length = 0;
        }
        return true;
    }
    size_t length = 0;
    while (length <= maximum && value[length] != '\0') {
        length++;
    }
    if (length > maximum) {
        return false;
    }
    if (out_length != NULL) {
        *out_length = length;
    }
    return true;
}

static esp_err_t account_owned_string(size_t *owned_bytes, const char *value, size_t field_maximum)
{
    if (value == NULL) {
        return ESP_OK;
    }
    size_t remaining = *owned_bytes <= CONFIG_OTOOL_LLM_MAX_REQUEST_BYTES
                           ? CONFIG_OTOOL_LLM_MAX_REQUEST_BYTES - *owned_bytes
                           : 0;
    size_t scan_maximum = field_maximum < remaining ? field_maximum : remaining;
    size_t length = 0;
    if (!bounded_string_length(value, scan_maximum, &length) || length == remaining) {
        return OTOOL_LLM_ERR_REQUEST_TOO_LARGE;
    }
    *owned_bytes += length + 1;
    return ESP_OK;
}

static bool tool_name_valid(const char *name)
{
    size_t length = 0;
    if (name == NULL || name[0] == '\0' ||
        !bounded_string_length(name, CONFIG_OTOOL_LLM_MAX_TOOL_NAME_BYTES, &length)) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        if (!(isalnum((unsigned char)name[i]) || name[i] == '_')) {
            return false;
        }
    }
    return true;
}

static esp_err_t request_validate_and_measure(const otool_llm_text_request_t *request,
                                              otool_llm_protocol_t protocol)
{
    if (request->max_output_tokens < 0 ||
        (request->temperature_is_set && !isfinite(request->temperature))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (request->message_count > CONFIG_OTOOL_LLM_MAX_REQUEST_MESSAGES) {
        return OTOOL_LLM_ERR_REQUEST_TOO_LARGE;
    }
    if (request->tool_count > CONFIG_OTOOL_LLM_MAX_TOOLS ||
        request->tool_output_count > CONFIG_OTOOL_LLM_MAX_TOOLS) {
        return OTOOL_LLM_ERR_TOOL_SCHEMA;
    }

    size_t owned_bytes = 0;
    esp_err_t err = account_owned_string(&owned_bytes, request->model, OTOOL_LLM_MAX_MODEL_BYTES);
    if (err == ESP_OK) {
        err = account_owned_string(&owned_bytes, request->instructions,
                                   CONFIG_OTOOL_LLM_MAX_REQUEST_BYTES);
    }
    if (err == ESP_OK) {
        err = account_owned_string(&owned_bytes, request->previous_response_id,
                                   OTOOL_LLM_MAX_PROVIDER_ID_BYTES);
    }
    if (err != ESP_OK) {
        return err;
    }

    for (size_t i = 0; i < request->message_count; i++) {
        const otool_llm_text_message_t *message = &request->messages[i];
        if (message->role < OTOOL_LLM_ROLE_DEVELOPER || message->role > OTOOL_LLM_ROLE_TOOL ||
            message->text == NULL ||
            (message->tool_call_count > 0 && message->tool_calls == NULL) ||
            message->tool_call_count > CONFIG_OTOOL_LLM_MAX_TOOLS) {
            return ESP_ERR_INVALID_ARG;
        }
        if (protocol == OTOOL_LLM_PROTOCOL_RESPONSES_SSE &&
            (message->role == OTOOL_LLM_ROLE_TOOL || message->tool_call_count > 0 ||
             message->tool_call_id != NULL)) {
            return OTOOL_LLM_ERR_UNSUPPORTED;
        }
        if (message->role == OTOOL_LLM_ROLE_TOOL) {
            size_t ignored = 0;
            if (message->tool_call_id == NULL || message->tool_call_id[0] == '\0' ||
                !bounded_string_length(message->tool_call_id,
                                       OTOOL_LLM_MAX_TOOL_CALL_ID_BYTES, &ignored)) {
                return ESP_ERR_INVALID_ARG;
            }
        } else if (message->tool_call_id != NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        if (message->tool_call_count > 0 && message->role != OTOOL_LLM_ROLE_ASSISTANT) {
            return ESP_ERR_INVALID_ARG;
        }
        err = account_owned_string(&owned_bytes, message->text,
                                   CONFIG_OTOOL_LLM_MAX_REQUEST_BYTES);
        if (err == ESP_OK) {
            err = account_owned_string(&owned_bytes, message->tool_call_id,
                                       OTOOL_LLM_MAX_TOOL_CALL_ID_BYTES);
        }
        if (err != ESP_OK) {
            return err;
        }
        for (size_t j = 0; j < message->tool_call_count; j++) {
            const otool_llm_tool_call_msg_t *call = &message->tool_calls[j];
            size_t ignored = 0;
            if (call->id == NULL || call->id[0] == '\0' ||
                !bounded_string_length(call->id, OTOOL_LLM_MAX_TOOL_CALL_ID_BYTES, &ignored) ||
                !tool_name_valid(call->name) || call->arguments == NULL) {
                return ESP_ERR_INVALID_ARG;
            }
            err = account_owned_string(&owned_bytes, call->id,
                                       OTOOL_LLM_MAX_TOOL_CALL_ID_BYTES);
            if (err == ESP_OK) {
                err = account_owned_string(&owned_bytes, call->name,
                                           CONFIG_OTOOL_LLM_MAX_TOOL_NAME_BYTES);
            }
            if (err == ESP_OK) {
                err = account_owned_string(&owned_bytes, call->arguments,
                                           CONFIG_OTOOL_LLM_MAX_TOOL_ARGUMENT_BYTES);
            }
            if (err != ESP_OK) {
                return err;
            }
        }
    }

    size_t total_schema_bytes = 0;
    for (size_t i = 0; i < request->tool_count; i++) {
        const otool_llm_tool_definition_t *tool = &request->tools[i];
        size_t description_len = 0;
        size_t schema_len = 0;
        if (tool->struct_size < sizeof(*tool)) {
            return ESP_ERR_INVALID_VERSION;
        }
        if (!tool_name_valid(tool->name)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (!bounded_string_length(tool->description,
                                   CONFIG_OTOOL_LLM_MAX_TOOL_DESCRIPTION_BYTES,
                                   &description_len)) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (tool->parameters_json_schema == NULL ||
            !bounded_string_length(tool->parameters_json_schema,
                                   CONFIG_OTOOL_LLM_MAX_TOOL_SCHEMA_BYTES, &schema_len) ||
            schema_len > CONFIG_OTOOL_LLM_MAX_TOTAL_TOOL_SCHEMA_BYTES - total_schema_bytes) {
            return OTOOL_LLM_ERR_TOOL_SCHEMA;
        }
        if (tool->max_output_bytes > CONFIG_OTOOL_LLM_MAX_TOOL_OUTPUT_BYTES) {
            return OTOOL_LLM_ERR_TOOL_OUTPUT_TOO_LARGE;
        }
        for (size_t j = 0; j < i; j++) {
            if (strcmp(request->tools[j].name, tool->name) == 0) {
                return ESP_ERR_INVALID_ARG;
            }
        }
        err = otool_llm_tool_schema_validate(tool->parameters_json_schema, schema_len,
                                             tool->strict);
        if (err != ESP_OK) {
            return err;
        }
        total_schema_bytes += schema_len;
        err = account_owned_string(&owned_bytes, tool->name,
                                   CONFIG_OTOOL_LLM_MAX_TOOL_NAME_BYTES);
        if (err == ESP_OK) {
            err = account_owned_string(&owned_bytes, tool->description,
                                       CONFIG_OTOOL_LLM_MAX_TOOL_DESCRIPTION_BYTES);
        }
        if (err == ESP_OK) {
            err = account_owned_string(&owned_bytes, tool->parameters_json_schema,
                                       CONFIG_OTOOL_LLM_MAX_TOOL_SCHEMA_BYTES);
        }
        if (err != ESP_OK) {
            return err;
        }
    }

    for (size_t i = 0; i < request->tool_output_count; i++) {
        const otool_llm_tool_output_t *output = &request->tool_outputs[i];
        size_t ignored = 0;
        if (output->call_id == NULL || output->call_id[0] == '\0' || output->output == NULL ||
            !bounded_string_length(output->call_id, OTOOL_LLM_MAX_TOOL_CALL_ID_BYTES, &ignored)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (!bounded_string_length(output->output, CONFIG_OTOOL_LLM_MAX_TOOL_OUTPUT_BYTES,
                                   &ignored)) {
            return OTOOL_LLM_ERR_TOOL_OUTPUT_TOO_LARGE;
        }
        err = account_owned_string(&owned_bytes, output->call_id,
                                   OTOOL_LLM_MAX_TOOL_CALL_ID_BYTES);
        if (err == ESP_OK) {
            err = account_owned_string(&owned_bytes, output->output,
                                       CONFIG_OTOOL_LLM_MAX_TOOL_OUTPUT_BYTES);
        }
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t otool_llm_request_create(otool_llm_client_handle_t client,
                                   const otool_llm_text_request_t *request,
                                   otool_llm_request_handle_t *out_request)
{
    if (out_request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_request = NULL;
    if (client == NULL || request == NULL) {
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
        if (request->tool_output_count > 0) {
            /* function_call_output 是 Responses 概念；Chat 用 tool role 消息（WP5） */
            ESP_LOGE(TAG, "tool_outputs are not supported by the Chat protocol");
            return OTOOL_LLM_ERR_UNSUPPORTED;
        }
    }
    if (request->tool_count > 0) {
        if (request->tools == NULL) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (request->tool_output_count > 0 && request->tool_outputs == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = request_validate_and_measure(request, client->protocol->id);
    if (err != ESP_OK) {
        return err;
    }

    otool_llm_request_handle_t r = (otool_llm_request_handle_t)calloc(1, sizeof(*r));
    if (r == NULL) {
        return ESP_ERR_NO_MEM;
    }
    r->client = client;

    err = otool_llm_strdup(request->model, &r->model);
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
            /* Publish zero-initialized ownership immediately so partial OOM cleanup is complete. */
            r->message_count = request->message_count;
            for (size_t i = 0; i < request->message_count; i++) {
                r->messages[i].role = request->messages[i].role;
                err = otool_llm_strdup(request->messages[i].text, &r->messages[i].text);
                if (err == ESP_OK) {
                    err = otool_llm_strdup(request->messages[i].tool_call_id,
                                           &r->messages[i].tool_call_id);
                }
                /* assistant 消息的 tool_calls（WP5） */
                if (err == ESP_OK && request->messages[i].tool_calls != NULL &&
                    request->messages[i].tool_call_count > 0) {
                    r->messages[i].tool_calls = (otool_llm_request_tool_call_t *)calloc(
                        request->messages[i].tool_call_count, sizeof(*r->messages[i].tool_calls));
                    if (r->messages[i].tool_calls == NULL) {
                        err = ESP_ERR_NO_MEM;
                    } else {
                        r->messages[i].tool_call_count = request->messages[i].tool_call_count;
                        for (size_t j = 0; j < request->messages[i].tool_call_count; j++) {
                            const otool_llm_tool_call_msg_t *src =
                                &request->messages[i].tool_calls[j];
                            err = otool_llm_strdup(src->id,
                                                   (char **)&r->messages[i].tool_calls[j].id);
                            if (err == ESP_OK) {
                                err = otool_llm_strdup(src->name,
                                                       (char **)&r->messages[i].tool_calls[j].name);
                            }
                            if (err == ESP_OK) {
                                err = otool_llm_strdup(
                                    src->arguments,
                                    (char **)&r->messages[i].tool_calls[j].arguments);
                            }
                            if (err != ESP_OK) {
                                break;
                            }
                        }
                    }
                }
                if (err != ESP_OK) {
                    break;
                }
            }
        }
    }
    /* 深拷贝工具定义（WP2） */
    if (err == ESP_OK && request->tool_count > 0) {
        r->tools = (otool_llm_tool_definition_t *)calloc(request->tool_count, sizeof(*r->tools));
        if (r->tools == NULL) {
            err = ESP_ERR_NO_MEM;
        } else {
            r->tool_count = request->tool_count;
            for (size_t i = 0; i < request->tool_count; i++) {
                r->tools[i].struct_size = sizeof(*r->tools);
                r->tools[i].strict = request->tools[i].strict;
                r->tools[i].flags = request->tools[i].flags;
                r->tools[i].timeout_ms = request->tools[i].timeout_ms;
                r->tools[i].max_output_bytes = request->tools[i].max_output_bytes;
                r->tools[i].execute = request->tools[i].execute;
                r->tools[i].user_ctx = request->tools[i].user_ctx;
                err = otool_llm_strdup(request->tools[i].name, (char **)&r->tools[i].name);
                if (err == ESP_OK) {
                    err = otool_llm_strdup(request->tools[i].description,
                                           (char **)&r->tools[i].description);
                }
                if (err == ESP_OK) {
                    err = otool_llm_strdup(request->tools[i].parameters_json_schema,
                                           (char **)&r->tools[i].parameters_json_schema);
                }
                if (err != ESP_OK) {
                    break;
                }
            }
        }
    }
    /* 深拷贝工具输出（function_call_output） */
    if (err == ESP_OK && request->tool_output_count > 0) {
        r->tool_outputs = (otool_llm_tool_output_t *)calloc(request->tool_output_count,
                                                            sizeof(*r->tool_outputs));
        if (r->tool_outputs == NULL) {
            err = ESP_ERR_NO_MEM;
        } else {
            r->tool_output_count = request->tool_output_count;
            for (size_t i = 0; i < request->tool_output_count; i++) {
                err = otool_llm_strdup(request->tool_outputs[i].call_id,
                                       (char **)&r->tool_outputs[i].call_id);
                if (err == ESP_OK) {
                    err = otool_llm_strdup(request->tool_outputs[i].output,
                                           (char **)&r->tool_outputs[i].output);
                }
                if (err != ESP_OK) {
                    break;
                }
            }
        }
    }
    if (err != ESP_OK) {
        request_data_free(r);
        free(r);
        return err;
    }

    r->max_output_tokens = request->max_output_tokens;
    r->temperature = request->temperature;
    r->temperature_is_set = request->temperature_is_set;
    r->store = request->store;

    r->lock = xSemaphoreCreateMutex();
    if (r->lock == NULL) {
        request_data_free(r);
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

    request_data_free(request);
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
        if (xSemaphoreTake(request->lock, portMAX_DELAY) == pdTRUE) {
            request->executing = false;
            xSemaphoreGive(request->lock);
        }
        return ESP_ERR_INVALID_STATE;
    }
    if (client->active != NULL) {
        xSemaphoreGive(client->lock);
        if (xSemaphoreTake(request->lock, portMAX_DELAY) == pdTRUE) {
            request->executing = false;
            xSemaphoreGive(request->lock);
        }
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
        .tools = request->tools,
        .tool_count = request->tool_count,
        .tool_outputs = request->tool_outputs,
        .tool_output_count = request->tool_output_count,
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
    char auth[1024] = { 0 };

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
    ESP_LOGI(TAG, "request body: %u bytes, tools=%u, messages=%u, outputs=%u",
             (unsigned)body_len, (unsigned)view.tool_count, (unsigned)view.message_count,
             (unsigned)view.tool_output_count);

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

    otool_llm_secure_zero(auth, sizeof(auth));
    free(url);
    otool_llm_secure_zero(body, body_cap);
    free(body);
    goto out;

out_error_local:
    /* Local failure (OOM, request too large, bad URL, auth build): emit a terminal ERROR. */
    if (!ctx->terminal_sent) {
        otool_llm_exec_report_error(ctx, err, esp_err_to_name(err), NULL);
    }
    otool_llm_secure_zero(auth, sizeof(auth));
    free(url);
    if (body != NULL) {
        otool_llm_secure_zero(body, body_cap);
    }
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
