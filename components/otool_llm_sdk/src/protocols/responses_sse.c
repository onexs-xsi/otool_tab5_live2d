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
    } else if (strcmp(type, "response.completed") == 0) {
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
