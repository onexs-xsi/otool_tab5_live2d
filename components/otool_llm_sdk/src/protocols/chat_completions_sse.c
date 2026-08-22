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
    size_t len = strlen(item->valuestring);
    if (len >= out_size) {
        out[0] = '\0';
        return false;
    }
    memcpy(out, item->valuestring, len + 1);
    return true;
}

static bool parse_tool_index(cJSON *tool_call, uint32_t *out)
{
    cJSON *item = get_child(tool_call, "index");
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > UINT32_MAX) {
        return false;
    }
    uint32_t value = (uint32_t)item->valuedouble;
    if ((double)value != item->valuedouble) {
        return false;
    }
    *out = value;
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
    case OTOOL_LLM_ROLE_TOOL:
        return "tool";
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
        if (req->messages[i].role == OTOOL_LLM_ROLE_TOOL) {
            /* tool 消息：需要 tool_call_id，content 承载结果 */
            if (req->messages[i].tool_call_id != NULL) {
                cJSON_AddStringToObject(m, "tool_call_id", req->messages[i].tool_call_id);
            }
            cJSON_AddStringToObject(m, "content",
                                    req->messages[i].text != NULL ? req->messages[i].text : "");
        } else {
            cJSON_AddStringToObject(m, "content",
                                    req->messages[i].text != NULL ? req->messages[i].text : "");
            /* assistant 消息可携带 tool_calls（WP5） */
            if (req->messages[i].tool_calls != NULL && req->messages[i].tool_call_count > 0) {
                cJSON *calls = cJSON_AddArrayToObject(m, "tool_calls");
                for (size_t j = 0; j < req->messages[i].tool_call_count; j++) {
                    const otool_llm_request_tool_call_t *tc = &req->messages[i].tool_calls[j];
                    cJSON *call = cJSON_CreateObject();
                    cJSON_AddStringToObject(call, "id", tc->id != NULL ? tc->id : "");
                    cJSON_AddStringToObject(call, "type", "function");
                    cJSON *fn = cJSON_CreateObject();
                    cJSON_AddStringToObject(fn, "name", tc->name != NULL ? tc->name : "");
                    cJSON_AddStringToObject(fn, "arguments",
                                            tc->arguments != NULL ? tc->arguments : "");
                    cJSON_AddItemToObject(call, "function", fn);
                    cJSON_AddItemToArray(calls, call);
                }
            }
        }
        cJSON_AddItemToArray(messages, m);
    }
    cJSON_AddBoolToObject(root, "stream", 1);
    if (provider->capabilities & OTOOL_LLM_PROVIDER_CAP_CHAT_STREAM_OPTIONS) {
        cJSON *stream_options = cJSON_AddObjectToObject(root, "stream_options");
        cJSON_AddBoolToObject(stream_options, "include_usage", 1);
    }
    /* 工具定义（WP5）：tools[].function */
    if (req->tool_count > 0) {
        cJSON *tools = cJSON_AddArrayToObject(root, "tools");
        for (size_t i = 0; i < req->tool_count; i++) {
            const otool_llm_tool_definition_t *t = &req->tools[i];
            cJSON *tool = cJSON_CreateObject();
            cJSON_AddStringToObject(tool, "type", "function");
            cJSON *fn = cJSON_CreateObject();
            cJSON_AddStringToObject(fn, "name", t->name);
            if (t->description != NULL) {
                cJSON_AddStringToObject(fn, "description", t->description);
            }
            cJSON *params = cJSON_Parse(t->parameters_json_schema);
            if (params == NULL) {
                cJSON_Delete(tool);
                cJSON_Delete(root);
                return OTOOL_LLM_ERR_TOOL_SCHEMA;
            }
            cJSON_AddItemToObject(fn, "parameters", params);
            cJSON_AddBoolToObject(fn, "strict", t->strict ? 1 : 0);
            cJSON_AddItemToObject(tool, "function", fn);
            cJSON_AddItemToArray(tools, tool);
        }
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
        /* Chat 流没有独立的 tool_calls.done 事件：参数流完 + finish_reason=tool_calls
         * 即完整。对仍 active 的槽补发 TOOL_CALL_DONE（计划 §4.3/§7）。 */
        otool_llm_chat_state_t *st = &ctx->proto.chat;
        for (int i = 0; i < CONFIG_OTOOL_LLM_MAX_PENDING_TOOL_CALLS; i++) {
            otool_llm_pending_tool_call_t *slot = &st->tool_calls[i];
            if (slot->active) {
                if (strcmp(st->finish_reason, "tool_calls") != 0) {
                    return otool_llm_exec_report_error(
                        ctx, OTOOL_LLM_ERR_PROTOCOL,
                        "active tool call ended without finish_reason=tool_calls", NULL);
                }
                otool_llm_text_event_t evt = { .type = OTOOL_LLM_TEXT_EVENT_TOOL_CALL_DONE };
                evt.data.tool_call_done.output_index = slot->output_index;
                evt.data.tool_call_done.call_id = slot->call_id[0] ? slot->call_id : NULL;
                evt.data.tool_call_done.name = slot->name[0] ? slot->name : NULL;
                evt.data.tool_call_done.arguments = slot->arguments;
                evt.data.tool_call_done.arguments_len = slot->arguments_len;
                ctx->emit(ctx, &evt);
                slot->active = false;
            }
        }
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
                if (!copy_string_field(choice, "finish_reason", ctx->proto.chat.finish_reason,
                                       sizeof(ctx->proto.chat.finish_reason))) {
                    err = otool_llm_exec_report_error(ctx, OTOOL_LLM_ERR_PROTOCOL,
                                                      "finish_reason over budget", NULL);
                    goto out;
                }
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
            /* 流式工具调用（WP5）：delta.tool_calls[] 按 index 聚合 */
            cJSON *tool_calls = get_child(delta, "tool_calls");
            if (cJSON_IsArray(tool_calls)) {
                int tc_count = cJSON_GetArraySize(tool_calls);
                for (int j = 0; j < tc_count; j++) {
                    cJSON *tc = cJSON_GetArrayItem(tool_calls, j);
                    uint32_t index = 0;
                    if (!parse_tool_index(tc, &index)) {
                        err = otool_llm_exec_report_error(ctx, OTOOL_LLM_ERR_JSON,
                                                          "tool call has invalid index", NULL);
                        goto out;
                    }
                    otool_llm_chat_state_t *st = &ctx->proto.chat;
                    otool_llm_pending_tool_call_t *slot = NULL;
                    for (int k = 0; k < CONFIG_OTOOL_LLM_MAX_PENDING_TOOL_CALLS; k++) {
                        if (st->tool_calls[k].active && st->tool_calls[k].output_index == index) {
                            slot = &st->tool_calls[k];
                            break;
                        }
                    }
                    cJSON *tc_id = get_child(tc, "id");
                    cJSON *tc_fn = get_child(tc, "function");
                    cJSON *fn_name = get_child(tc_fn, "name");
                    cJSON *fn_args = get_child(tc_fn, "arguments");
                    bool has_id = cJSON_IsString(tc_id) && tc_id->valuestring != NULL;
                    bool has_name = cJSON_IsString(fn_name) && fn_name->valuestring != NULL;
                    bool is_new = has_id || has_name;
                    if (slot == NULL && is_new) {
                        if (!has_id || !has_name) {
                            err = otool_llm_exec_report_error(
                                ctx, OTOOL_LLM_ERR_PROTOCOL,
                                "new tool call requires id and function name", NULL);
                            goto out;
                        }
                        for (int k = 0; k < CONFIG_OTOOL_LLM_MAX_PENDING_TOOL_CALLS; k++) {
                            if (!st->tool_calls[k].active) {
                                slot = &st->tool_calls[k];
                                break;
                            }
                        }
                        if (slot == NULL) {
                            err = otool_llm_exec_report_error(ctx, OTOOL_LLM_ERR_TOOL_ARGUMENTS,
                                                              "too many pending tool calls", NULL);
                            goto out;
                        }
                        memset(slot, 0, sizeof(*slot));
                        slot->active = true;
                        slot->output_index = index;
                        if (!copy_string_field(tc, "id", slot->call_id,
                                               sizeof(slot->call_id)) ||
                            !copy_string_field(tc_fn, "name", slot->name,
                                               sizeof(slot->name))) {
                            err = otool_llm_exec_report_error(
                                ctx, OTOOL_LLM_ERR_PROTOCOL,
                                "tool identity missing or over budget", NULL);
                            goto out;
                        }
                        otool_llm_text_event_t evt = {
                            .type = OTOOL_LLM_TEXT_EVENT_TOOL_CALL_STARTED
                        };
                        evt.data.tool_call_started.output_index = index;
                        evt.data.tool_call_started.call_id =
                            slot->call_id[0] ? slot->call_id : NULL;
                        evt.data.tool_call_started.name = slot->name[0] ? slot->name : NULL;
                        ctx->emit(ctx, &evt);
                    }
                    if (slot != NULL &&
                        ((has_id && strcmp(slot->call_id, tc_id->valuestring) != 0) ||
                         (has_name && strcmp(slot->name, fn_name->valuestring) != 0))) {
                        err = otool_llm_exec_report_error(
                            ctx, OTOOL_LLM_ERR_PROTOCOL,
                            "tool identity changed for an active index", NULL);
                        goto out;
                    }
                    if (slot == NULL) {
                        err = otool_llm_exec_report_error(
                            ctx, OTOOL_LLM_ERR_PROTOCOL,
                            "tool delta without active tool call", NULL);
                        goto out;
                    }
                    if (slot != NULL && cJSON_IsString(fn_args) && fn_args->valuestring != NULL) {
                        size_t alen = strlen(fn_args->valuestring);
                        if (slot->arguments_len + alen >= sizeof(slot->arguments)) {
                            err = otool_llm_exec_report_error(ctx, OTOOL_LLM_ERR_TOOL_ARGUMENTS,
                                                              "tool arguments over budget", NULL);
                            goto out;
                        }
                        memcpy(slot->arguments + slot->arguments_len, fn_args->valuestring, alen);
                        slot->arguments_len += alen;
                        slot->arguments[slot->arguments_len] = '\0';
                        otool_llm_text_event_t evt = {
                            .type = OTOOL_LLM_TEXT_EVENT_TOOL_ARGUMENTS_DELTA
                        };
                        evt.data.tool_arguments_delta.output_index = index;
                        evt.data.tool_arguments_delta.call_id =
                            slot->call_id[0] ? slot->call_id : NULL;
                        evt.data.tool_arguments_delta.delta = fn_args->valuestring;
                        evt.data.tool_arguments_delta.delta_len = alen;
                        ctx->emit(ctx, &evt);
                    }
                }
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
