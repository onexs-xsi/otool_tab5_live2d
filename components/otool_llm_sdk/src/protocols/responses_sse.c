/*
 * SPDX-FileCopyrightText: 2026 otool project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * OpenAI Responses API SSE adapter. Pure cJSON + event mapping; no HTTP knowledge.
 */

#include "otool_llm_protocol.h"

#include "cJSON.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "otool_llm_responses";

/* ---------------- helpers ---------------- */

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

static void parse_usage(cJSON *usage_obj, otool_llm_usage_t *usage)
{
    usage->input_tokens = -1;
    usage->output_tokens = -1;
    usage->total_tokens = -1;
    if (!cJSON_IsObject(usage_obj)) {
        return;
    }
    cJSON *in = get_child(usage_obj, "input_tokens");
    cJSON *out = get_child(usage_obj, "output_tokens");
    cJSON *total = get_child(usage_obj, "total_tokens");
    if (cJSON_IsNumber(in)) {
        usage->input_tokens = (int64_t)in->valuedouble;
    }
    if (cJSON_IsNumber(out)) {
        usage->output_tokens = (int64_t)out->valuedouble;
    }
    if (cJSON_IsNumber(total)) {
        usage->total_tokens = (int64_t)total->valuedouble;
    }
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

static esp_err_t responses_build_request(const otool_llm_request_view_t *req,
                                         const otool_llm_provider_preset_t *provider,
                                         char *out, size_t out_size, size_t *out_len)
{
    (void)provider;
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "model", req->model);

    if (req->message_count > 0) {
        cJSON *input = cJSON_AddArrayToObject(root, "input");
        for (size_t i = 0; i < req->message_count; i++) {
            cJSON *m = cJSON_CreateObject();
            if (m == NULL) {
                cJSON_Delete(root);
                return ESP_ERR_NO_MEM;
            }
            cJSON_AddStringToObject(m, "role", role_name(req->messages[i].role));
            cJSON_AddStringToObject(m, "content", req->messages[i].text);
            cJSON_AddItemToArray(input, m);
        }
    }
    if (req->instructions != NULL) {
        cJSON_AddStringToObject(root, "instructions", req->instructions);
    }
    cJSON_AddBoolToObject(root, "stream", 1);
    cJSON_AddBoolToObject(root, "store", req->store ? 1 : 0);
    if (req->previous_response_id != NULL) {
        cJSON_AddStringToObject(root, "previous_response_id", req->previous_response_id);
    }
    if (req->max_output_tokens > 0) {
        cJSON_AddNumberToObject(root, "max_output_tokens", req->max_output_tokens);
    }
    if (req->temperature_is_set) {
        cJSON_AddNumberToObject(root, "temperature", (double)req->temperature);
    }

    /* ---- function calling（WP2） ---- */
    if (req->tool_count > 0) {
        if (req->tools == NULL) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        cJSON *tools = cJSON_AddArrayToObject(root, "tools");
        for (size_t i = 0; i < req->tool_count; i++) {
            const otool_llm_tool_definition_t *t = &req->tools[i];
            if (t->name == NULL || t->name[0] == '\0' || t->parameters_json_schema == NULL) {
                cJSON_Delete(root);
                return OTOOL_LLM_ERR_TOOL_SCHEMA;
            }
            cJSON *tool = cJSON_CreateObject();
            if (tool == NULL) {
                cJSON_Delete(root);
                return ESP_ERR_NO_MEM;
            }
            cJSON_AddStringToObject(tool, "type", "function");
            cJSON_AddStringToObject(tool, "name", t->name);
            if (t->description != NULL) {
                cJSON_AddStringToObject(tool, "description", t->description);
            }
            /* parameters: 解析 JSON Schema 字符串后嵌入 */
            cJSON *params = cJSON_Parse(t->parameters_json_schema);
            if (params == NULL) {
                cJSON_Delete(tool);
                cJSON_Delete(root);
                return OTOOL_LLM_ERR_TOOL_SCHEMA;
            }
            cJSON_AddItemToObject(tool, "parameters", params);
            cJSON_AddBoolToObject(tool, "strict", t->strict ? 1 : 0);
            cJSON_AddItemToArray(tools, tool);
        }
        cJSON_AddStringToObject(root, "tool_choice", "auto");
        cJSON_AddBoolToObject(root, "parallel_tool_calls", 0);
    }
    if (req->tool_output_count > 0) {
        if (req->tool_outputs == NULL) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        cJSON *input = cJSON_GetObjectItemCaseSensitive(root, "input");
        if (input == NULL) {
            input = cJSON_AddArrayToObject(root, "input");
        }
        for (size_t i = 0; i < req->tool_output_count; i++) {
            const otool_llm_tool_output_t *o = &req->tool_outputs[i];
            if (o->call_id == NULL || o->output == NULL) {
                cJSON_Delete(root);
                return ESP_ERR_INVALID_ARG;
            }
            cJSON *item = cJSON_CreateObject();
            if (item == NULL) {
                cJSON_Delete(root);
                return ESP_ERR_NO_MEM;
            }
            cJSON_AddStringToObject(item, "type", "function_call_output");
            cJSON_AddStringToObject(item, "call_id", o->call_id);
            cJSON_AddStringToObject(item, "output", o->output);
            cJSON_AddItemToArray(input, item);
        }
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

static esp_err_t responses_on_sse_event(otool_llm_exec_ctx_t *ctx,
                                        const char *event_name,
                                        const char *data, size_t data_len)
{
    /* 方舟在 Responses 流末尾也发送 data: [DONE]（OpenAI 官方 Responses 不发送；
     * 这是 Ark 与 OpenAI 的协议差异，记录于实施计划 §15.6）。
     * 它只是流结束标记，不作为 JSON 解析，也不伪造完成事件——
     * 正常终止仍由 response.completed / response.incomplete 决定。 */
    if (data_len == 6 && memcmp(data, "[DONE]", 6) == 0) {
        return ESP_OK;
    }

    cJSON *root = cJSON_ParseWithLength(data, data_len);
    if (root == NULL) {
        ESP_LOGE(TAG, "invalid JSON in SSE event '%s'", event_name);
        return otool_llm_exec_report_error(ctx, OTOOL_LLM_ERR_JSON, "invalid JSON in SSE event", NULL);
    }

    esp_err_t err = ESP_OK;
    const char *type = event_name != NULL ? event_name : "message";

    if (strcmp(type, "response.created") == 0) {
        cJSON *resp = get_child(root, "response");
        copy_string_field(resp, "id", ctx->response_id, sizeof(ctx->response_id));
        copy_string_field(resp, "model", ctx->model, sizeof(ctx->model));
        if (!ctx->proto.responses.started) {
            ctx->proto.responses.started = true;
            otool_llm_text_event_t evt = { .type = OTOOL_LLM_TEXT_EVENT_RESPONSE_STARTED };
            ctx->emit(ctx, &evt);
        }
    } else if (strcmp(type, "response.output_text.delta") == 0) {
        cJSON *delta = get_child(root, "delta");
        if (!cJSON_IsString(delta) || delta->valuestring == NULL) {
            err = otool_llm_exec_report_error(ctx, OTOOL_LLM_ERR_JSON,
                                              "response.output_text.delta missing string delta", NULL);
            goto out;
        }
        otool_llm_text_event_t evt = {
            .type = OTOOL_LLM_TEXT_EVENT_TEXT_DELTA,
        };
        evt.data.text_delta.data = delta->valuestring;
        evt.data.text_delta.data_len = strlen(delta->valuestring);
        ctx->emit(ctx, &evt);
    } else if (strcmp(type, "response.output_text.done") == 0) {
        /* Do not resend the full text; the app concatenated deltas already. */
        otool_llm_text_event_t evt = { .type = OTOOL_LLM_TEXT_EVENT_TEXT_DONE };
        ctx->emit(ctx, &evt);
    } else if (strcmp(type, "response.output_item.added") == 0) {
        cJSON *item = get_child(root, "item");
        cJSON *itype = get_child(item, "type");
        if (cJSON_IsString(itype) && itype->valuestring != NULL &&
            strcmp(itype->valuestring, "function_call") == 0) {
            /* 新 function_call item：按 output_index 分配槽位（WP2） */
            cJSON *oi = get_child(root, "output_index");
            if (!cJSON_IsNumber(oi)) {
                err = otool_llm_exec_report_error(ctx, OTOOL_LLM_ERR_JSON,
                                                  "output_item.added missing output_index", NULL);
                goto out;
            }
            uint32_t index = (uint32_t)oi->valuedouble;
            otool_llm_responses_state_t *st = &ctx->proto.responses;
            otool_llm_pending_tool_call_t *slot = NULL;
            for (int i = 0; i < CONFIG_OTOOL_LLM_MAX_PENDING_TOOL_CALLS; i++) {
                if (!st->tool_calls[i].active) {
                    slot = &st->tool_calls[i];
                    break;
                }
                if (st->tool_calls[i].output_index == index) {
                    slot = &st->tool_calls[i];
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
            copy_string_field(item, "id", slot->item_id, sizeof(slot->item_id));
            copy_string_field(item, "call_id", slot->call_id, sizeof(slot->call_id));
            copy_string_field(item, "name", slot->name, sizeof(slot->name));
            otool_llm_text_event_t evt = { .type = OTOOL_LLM_TEXT_EVENT_TOOL_CALL_STARTED };
            evt.data.tool_call_started.output_index = index;
            evt.data.tool_call_started.item_id = slot->item_id[0] ? slot->item_id : NULL;
            evt.data.tool_call_started.call_id = slot->call_id[0] ? slot->call_id : NULL;
            evt.data.tool_call_started.name = slot->name[0] ? slot->name : NULL;
            ctx->emit(ctx, &evt);
        }
        /* reasoning/message item: 忽略（文本由 output_text.delta 事件承载） */
    } else if (strcmp(type, "response.function_call_arguments.delta") == 0) {
        cJSON *oi = get_child(root, "output_index");
        cJSON *delta = get_child(root, "delta");
        if (!cJSON_IsNumber(oi)) {
            err = otool_llm_exec_report_error(ctx, OTOOL_LLM_ERR_JSON,
                                              "function_call_arguments.delta missing output_index", NULL);
            goto out;
        }
        if (!cJSON_IsString(delta) || delta->valuestring == NULL) {
            err = otool_llm_exec_report_error(ctx, OTOOL_LLM_ERR_JSON,
                                              "function_call_arguments.delta missing string delta", NULL);
            goto out;
        }
        uint32_t index = (uint32_t)oi->valuedouble;
        otool_llm_responses_state_t *st = &ctx->proto.responses;
        otool_llm_pending_tool_call_t *slot = NULL;
        for (int i = 0; i < CONFIG_OTOOL_LLM_MAX_PENDING_TOOL_CALLS; i++) {
            if (st->tool_calls[i].active && st->tool_calls[i].output_index == index) {
                slot = &st->tool_calls[i];
                break;
            }
        }
        if (slot == NULL) {
            err = otool_llm_exec_report_error(ctx, OTOOL_LLM_ERR_PROTOCOL,
                                              "arguments delta without active tool call", NULL);
            goto out;
        }
        size_t dlen = strlen(delta->valuestring);
        if (slot->arguments_len + dlen >= sizeof(slot->arguments)) {
            err = otool_llm_exec_report_error(ctx, OTOOL_LLM_ERR_TOOL_ARGUMENTS,
                                              "tool arguments over budget", NULL);
            goto out;
        }
        memcpy(slot->arguments + slot->arguments_len, delta->valuestring, dlen);
        slot->arguments_len += dlen;
        slot->arguments[slot->arguments_len] = '\0';
        otool_llm_text_event_t evt = { .type = OTOOL_LLM_TEXT_EVENT_TOOL_ARGUMENTS_DELTA };
        evt.data.tool_arguments_delta.output_index = index;
        evt.data.tool_arguments_delta.call_id = slot->call_id[0] ? slot->call_id : NULL;
        evt.data.tool_arguments_delta.delta = delta->valuestring;
        evt.data.tool_arguments_delta.delta_len = dlen;
        ctx->emit(ctx, &evt);
    } else if (strcmp(type, "response.function_call_arguments.done") == 0) {
        cJSON *oi = get_child(root, "output_index");
        cJSON *args = get_child(root, "arguments");
        if (!cJSON_IsNumber(oi)) {
            err = otool_llm_exec_report_error(ctx, OTOOL_LLM_ERR_JSON,
                                              "function_call_arguments.done missing output_index", NULL);
            goto out;
        }
        if (!cJSON_IsString(args) || args->valuestring == NULL) {
            err = otool_llm_exec_report_error(ctx, OTOOL_LLM_ERR_JSON,
                                              "function_call_arguments.done missing arguments", NULL);
            goto out;
        }
        uint32_t index = (uint32_t)oi->valuedouble;
        otool_llm_responses_state_t *st = &ctx->proto.responses;
        otool_llm_pending_tool_call_t *slot = NULL;
        for (int i = 0; i < CONFIG_OTOOL_LLM_MAX_PENDING_TOOL_CALLS; i++) {
            if (st->tool_calls[i].active && st->tool_calls[i].output_index == index) {
                slot = &st->tool_calls[i];
                break;
            }
        }
        if (slot == NULL) {
            err = otool_llm_exec_report_error(ctx, OTOOL_LLM_ERR_PROTOCOL,
                                              "arguments done without active tool call", NULL);
            goto out;
        }
        /* 完整性校验：done 的完整 arguments 必须与累计 delta 拼接一致（计划 §7） */
        size_t alen = strlen(args->valuestring);
        if (alen != slot->arguments_len ||
            memcmp(args->valuestring, slot->arguments, alen) != 0) {
            err = otool_llm_exec_report_error(ctx, OTOOL_LLM_ERR_PROTOCOL,
                                              "arguments done mismatch accumulated deltas", NULL);
            goto out;
        }
        slot->arguments_done = true;
        otool_llm_text_event_t evt = { .type = OTOOL_LLM_TEXT_EVENT_TOOL_CALL_DONE };
        evt.data.tool_call_done.output_index = index;
        evt.data.tool_call_done.call_id = slot->call_id[0] ? slot->call_id : NULL;
        evt.data.tool_call_done.name = slot->name[0] ? slot->name : NULL;
        evt.data.tool_call_done.arguments = slot->arguments;
        evt.data.tool_call_done.arguments_len = slot->arguments_len;
        ctx->emit(ctx, &evt);
    } else if (strcmp(type, "response.output_item.done") == 0) {
        cJSON *item = get_child(root, "item");
        cJSON *itype = get_child(item, "type");
        if (cJSON_IsString(itype) && itype->valuestring != NULL &&
            strcmp(itype->valuestring, "function_call") == 0) {
            cJSON *oi = get_child(root, "output_index");
            uint32_t index = cJSON_IsNumber(oi) ? (uint32_t)oi->valuedouble : 0;
            otool_llm_responses_state_t *st = &ctx->proto.responses;
            for (int i = 0; i < CONFIG_OTOOL_LLM_MAX_PENDING_TOOL_CALLS; i++) {
                otool_llm_pending_tool_call_t *slot = &st->tool_calls[i];
                if (!slot->active || slot->output_index != index) {
                    continue;
                }
                if (!slot->arguments_done) {
                    /* 容错：provider 未发 arguments.done 时，用 item.arguments 补发 */
                    cJSON *args = get_child(item, "arguments");
                    if (cJSON_IsString(args) && args->valuestring != NULL) {
                        size_t alen = strlen(args->valuestring);
                        if (alen < sizeof(slot->arguments)) {
                            memcpy(slot->arguments, args->valuestring, alen + 1);
                            slot->arguments_len = alen;
                            slot->arguments_done = true;
                            otool_llm_text_event_t evt = {
                                .type = OTOOL_LLM_TEXT_EVENT_TOOL_CALL_DONE
                            };
                            evt.data.tool_call_done.output_index = index;
                            evt.data.tool_call_done.call_id = slot->call_id[0] ? slot->call_id : NULL;
                            evt.data.tool_call_done.name = slot->name[0] ? slot->name : NULL;
                            evt.data.tool_call_done.arguments = slot->arguments;
                            evt.data.tool_call_done.arguments_len = slot->arguments_len;
                            ctx->emit(ctx, &evt);
                        }
                    } else {
                        err = otool_llm_exec_report_error(ctx, OTOOL_LLM_ERR_PROTOCOL,
                                                          "tool call item done without arguments", NULL);
                        goto out;
                    }
                }
                slot->active = false; /* 槽位回收 */
                break;
            }
        }
    } else if (strcmp(type, "response.completed") == 0) {
        /* 终止前检查：不允许残留半个 tool call（计划 §7） */
        otool_llm_responses_state_t *st = &ctx->proto.responses;
        for (int i = 0; i < CONFIG_OTOOL_LLM_MAX_PENDING_TOOL_CALLS; i++) {
            if (st->tool_calls[i].active && !st->tool_calls[i].arguments_done) {
                err = otool_llm_exec_report_error(ctx, OTOOL_LLM_ERR_PROTOCOL,
                                                  "response completed with unfinished tool call", NULL);
                goto out;
            }
        }
        cJSON *resp = get_child(root, "response");
        cJSON *usage_obj = get_child(resp, "usage");
        if (cJSON_IsObject(usage_obj)) {
            parse_usage(usage_obj, &ctx->usage);
            ctx->usage_set = true;
            otool_llm_text_event_t evt = { .type = OTOOL_LLM_TEXT_EVENT_USAGE };
            evt.data.usage = ctx->usage;
            ctx->emit(ctx, &evt);
        }
        otool_llm_text_event_t evt = { .type = OTOOL_LLM_TEXT_EVENT_COMPLETED };
        ctx->emit(ctx, &evt);
    } else if (strcmp(type, "response.incomplete") == 0) {
        /* 终止前同样检查半工具调用 */
        otool_llm_responses_state_t *st = &ctx->proto.responses;
        for (int i = 0; i < CONFIG_OTOOL_LLM_MAX_PENDING_TOOL_CALLS; i++) {
            if (st->tool_calls[i].active && !st->tool_calls[i].arguments_done) {
                err = otool_llm_exec_report_error(ctx, OTOOL_LLM_ERR_PROTOCOL,
                                                  "response incomplete with unfinished tool call", NULL);
                goto out;
            }
        }
        cJSON *resp = get_child(root, "response");
        cJSON *usage_obj = get_child(resp, "usage");
        if (cJSON_IsObject(usage_obj)) {
            parse_usage(usage_obj, &ctx->usage);
            ctx->usage_set = true;
            otool_llm_text_event_t evt = { .type = OTOOL_LLM_TEXT_EVENT_USAGE };
            evt.data.usage = ctx->usage;
            ctx->emit(ctx, &evt);
        }
        cJSON *details = get_child(resp, "incomplete_details");
        char reason[64] = { 0 };
        copy_string_field(details, "reason", reason, sizeof(reason));
        otool_llm_text_event_t evt = {
            .type = OTOOL_LLM_TEXT_EVENT_INCOMPLETE,
        };
        evt.data.incomplete.reason = reason[0] != '\0' ? reason : NULL;
        ctx->emit(ctx, &evt);
    } else if (strcmp(type, "response.failed") == 0) {
        cJSON *resp = get_child(root, "response");
        cJSON *error_obj = get_child(resp, "error");
        char code[64] = { 0 };
        char message[192] = { 0 };
        copy_string_field(error_obj, "code", code, sizeof(code));
        copy_string_field(error_obj, "message", message, sizeof(message));
        err = otool_llm_exec_report_error(ctx, OTOOL_LLM_ERR_PROVIDER,
                                          message[0] ? message : "response failed",
                                          code[0] ? code : NULL);
    } else if (strcmp(type, "error") == 0) {
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
        err = otool_llm_exec_report_error(ctx, OTOOL_LLM_ERR_PROVIDER,
                                          message[0] ? message : "provider error",
                                          code[0] ? code : NULL);
    } else {
        /* Unknown events are tolerated and counted for debugging. */
        ESP_LOGD(TAG, "ignoring unknown SSE event '%s'", type);
    }

out:
    cJSON_Delete(root);
    return err;
}

static esp_err_t responses_on_eof(otool_llm_exec_ctx_t *ctx)
{
    return otool_llm_exec_report_error(ctx, OTOOL_LLM_ERR_PROTOCOL_EOF,
                                       "stream ended without a terminal event", NULL);
}

const otool_llm_protocol_ops_t otool_llm_protocol_responses = {
    .id = OTOOL_LLM_PROTOCOL_RESPONSES_SSE,
    .name = "responses_sse",
    .build_request = responses_build_request,
    .on_sse_event = responses_on_sse_event,
    .on_eof = responses_on_eof,
};
