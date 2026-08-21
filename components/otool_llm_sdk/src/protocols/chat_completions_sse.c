/*
 * SPDX-FileCopyrightText: 2026 otool project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Chat Completions SSE adapter (legacy / fallback protocol). Pure cJSON + event
 * mapping; no HTTP knowledge.
 */

#include "otool_llm_protocol.h"

#include "cJSON.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "otool_llm_chat";

static cJSON *get_child(cJSON *parent, const char *key)
{
    if (parent == NULL) {
        return NULL;
    }
    return cJSON_GetObjectItemCaseSensitive(parent, key);
}

static bool copy_string_field(cJSON *obj, const char *key, char *out, size_t out_size)
{
    cJSON *item = get_child(obj, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }
    snprintf(out, out_size, "%s", item->valuestring);
    return true;
}

/* ---------------- request building ---------------- */

static const char *role_name(otool_llm_role_t role)
{
    switch (role) {
    case OTOOL_LLM_ROLE_DEVELOPER:
        return "developer";
    case OTOOL_LLM_ROLE_SYSTEM:
        return "system";
    case OTOOL_LLM_ROLE_USER:
        return "user";
    case OTOOL_LLM_ROLE_ASSISTANT:
        return "assistant";
    default:
        return "user";
    }
}

static esp_err_t chat_build_request(const otool_llm_request_view_t *req,
                                    const otool_llm_provider_preset_t *provider,
                                    char *out, size_t out_size, size_t *out_len)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "model", req->model);
    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    if (req->instructions != NULL) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", "system");
        cJSON_AddStringToObject(m, "content", req->instructions);
        cJSON_AddItemToArray(messages, m);
    }
    for (size_t i = 0; i < req->message_count; i++) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", role_name(req->messages[i].role));
        cJSON_AddStringToObject(m, "content", req->messages[i].text);
        cJSON_AddItemToArray(messages, m);
    }
    cJSON_AddBoolToObject(root, "stream", 1);
    if (provider->capabilities & OTOOL_LLM_PROVIDER_CAP_CHAT_STREAM_OPTIONS) {
        cJSON *stream_options = cJSON_AddObjectToObject(root, "stream_options");
        cJSON_AddBoolToObject(stream_options, "include_usage", 1);
    }
    if (req->max_output_tokens > 0) {
        if (provider->capabilities & OTOOL_LLM_PROVIDER_CAP_CHAT_MAX_COMPLETION_TOKENS) {
            cJSON_AddNumberToObject(root, "max_completion_tokens", req->max_output_tokens);
        } else {
            cJSON_AddNumberToObject(root, "max_tokens", req->max_output_tokens);
        }
    }
    if (req->temperature_is_set) {
        cJSON_AddNumberToObject(root, "temperature", (double)req->temperature);
    }

    int written = cJSON_PrintPreallocated(root, out, (int)out_size, 0);
    cJSON_Delete(root);
    if (written == 0) {
        return OTOOL_LLM_ERR_REQUEST_TOO_LARGE;
    }
    *out_len = strlen(out);
    return ESP_OK;
}

/* ---------------- event mapping ---------------- */

static void chat_report_provider_error(otool_llm_exec_ctx_t *ctx, cJSON *root,
                                       esp_err_t *err_out)
{
    cJSON *error_obj = get_child(root, "error");
    char code[64] = { 0 };
    char message[192] = { 0 };
    copy_string_field(error_obj, "code", code, sizeof(code));
    copy_string_field(error_obj, "message", message, sizeof(message));
    if (message[0] == '\0') {
        copy_string_field(root, "message", message, sizeof(message));
    }
    if (code[0] == '\0') {
        copy_string_field(root, "code", code, sizeof(code));
    }
    copy_string_field(root, "request_id", ctx->request_id, sizeof(ctx->request_id));
    *err_out = otool_llm_exec_report_error(ctx, OTOOL_LLM_ERR_PROVIDER,
                                           message[0] ? message : "provider error",
                                           code[0] ? code : NULL);
}

static esp_err_t chat_on_sse_event(otool_llm_exec_ctx_t *ctx,
                                   const char *event_name,
                                   const char *data, size_t data_len)
{
    const char *type = event_name != NULL ? event_name : "message";

    if (strcmp(type, "error") == 0) {
        cJSON *root = cJSON_ParseWithLength(data, data_len);
        esp_err_t err = ESP_OK;
        if (root != NULL) {
            chat_report_provider_error(ctx, root, &err);
            cJSON_Delete(root);
        } else {
            err = otool_llm_exec_report_error(ctx, OTOOL_LLM_ERR_JSON, "invalid JSON in error event", NULL);
        }
        return err;
    }

    if (strcmp(type, "message") != 0) {
        ESP_LOGD(TAG, "ignoring unknown SSE event '%s'", type);
        return ESP_OK;
    }

    /* data: [DONE] */
    if (data_len == 6 && memcmp(data, "[DONE]", 6) == 0) {
        ctx->proto.chat.saw_done = true;
        otool_llm_text_event_t evt = { .type = OTOOL_LLM_TEXT_EVENT_COMPLETED };
        ctx->emit(ctx, &evt);
        return ESP_OK;
    }

    cJSON *root = cJSON_ParseWithLength(data, data_len);
    if (root == NULL) {
        ESP_LOGE(TAG, "invalid JSON in chat chunk");
        return otool_llm_exec_report_error(ctx, OTOOL_LLM_ERR_JSON, "invalid JSON in chat chunk", NULL);
    }

    esp_err_t err = ESP_OK;

    /* Provider error inside a chunk (e.g. mid-stream auth failure). */
    if (cJSON_IsObject(get_child(root, "error"))) {
        chat_report_provider_error(ctx, root, &err);
        goto out;
    }

    /* Usage chunk. */
    cJSON *usage_obj = get_child(root, "usage");
    if (cJSON_IsObject(usage_obj)) {
        ctx->proto.chat.saw_usage = true;
        ctx->usage.input_tokens = -1;
        ctx->usage.output_tokens = -1;
        ctx->usage.total_tokens = -1;
        cJSON *in = get_child(usage_obj, "prompt_tokens");
        cJSON *out = get_child(usage_obj, "completion_tokens");
        cJSON *total = get_child(usage_obj, "total_tokens");
        if (cJSON_IsNumber(in)) {
            ctx->usage.input_tokens = (int64_t)in->valuedouble;
        }
        if (cJSON_IsNumber(out)) {
            ctx->usage.output_tokens = (int64_t)out->valuedouble;
        }
        if (cJSON_IsNumber(total)) {
            ctx->usage.total_tokens = (int64_t)total->valuedouble;
        }
        ctx->usage_set = true;
        otool_llm_text_event_t evt = { .type = OTOOL_LLM_TEXT_EVENT_USAGE };
        evt.data.usage = ctx->usage;
        ctx->emit(ctx, &evt);
    }

    /* Choices (single choice only). */
    cJSON *choices = get_child(root, "choices");
    if (cJSON_IsArray(choices)) {
        int count = cJSON_GetArraySize(choices);
        if (count > 1) {
            err = otool_llm_exec_report_error(ctx, OTOOL_LLM_ERR_UNSUPPORTED,
                                              "multiple choices are not supported", NULL);
            goto out;
        }
        cJSON *choice = cJSON_GetArrayItem(choices, 0);
        if (choice != NULL) {
            cJSON *finish = get_child(choice, "finish_reason");
            if (cJSON_IsString(finish) && finish->valuestring != NULL) {
                snprintf(ctx->proto.chat.finish_reason, sizeof(ctx->proto.chat.finish_reason),
                         "%s", finish->valuestring);
            }
            cJSON *delta = get_child(choice, "delta");
            cJSON *content = get_child(delta, "content");
            if (cJSON_IsString(content)) {
                otool_llm_text_event_t evt = {
                    .type = OTOOL_LLM_TEXT_EVENT_TEXT_DELTA,
                };
                evt.data.text_delta.data = content->valuestring != NULL ? content->valuestring : "";
                evt.data.text_delta.data_len = content->valuestring != NULL ? strlen(content->valuestring) : 0;
                ctx->emit(ctx, &evt);
            }
        }
    }

out:
    cJSON_Delete(root);
    return err;
}

static esp_err_t chat_on_eof(otool_llm_exec_ctx_t *ctx)
{
    /* HTTP closed without [DONE]: protocol EOF, never a success. */
    return otool_llm_exec_report_error(ctx, OTOOL_LLM_ERR_PROTOCOL_EOF,
                                       "stream ended before [DONE]", NULL);
}

const otool_llm_protocol_ops_t otool_llm_protocol_chat = {
    .id = OTOOL_LLM_PROTOCOL_CHAT_COMPLETIONS_SSE,
    .name = "chat_completions_sse",
    .build_request = chat_build_request,
    .on_sse_event = chat_on_sse_event,
    .on_eof = chat_on_eof,
};
