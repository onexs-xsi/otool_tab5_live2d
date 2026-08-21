/*
 * SPDX-FileCopyrightText: 2026 otool project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "otool_llm_provider.h"

#include "cJSON.h"

#include <stdio.h>
#include <string.h>

static esp_err_t ark_build_auth_header(const char *api_key, char *out, size_t out_size)
{
    int n = snprintf(out, out_size, "Bearer %s", api_key);
    if (n < 0 || (size_t)n >= out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/* Ark error bodies look like {"error":{"code":"...","message":"...","request_id":"..."}}.
 * Some endpoints also return a bare {"code":...,"message":...}; handle both. */
static esp_err_t ark_parse_provider_error(const char *body, size_t len,
                                          char *out_message, size_t message_size,
                                          char *out_code, size_t code_size,
                                          char *out_request_id, size_t request_id_size)
{
    cJSON *root = cJSON_ParseWithLength(body, len);
    if (root == NULL) {
        if (out_message != NULL && message_size > 0) {
            snprintf(out_message, message_size, "%.*s", (int)(len < message_size - 1 ? len : message_size - 1), body);
        }
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *error_obj = cJSON_GetObjectItemCaseSensitive(root, "error");
    cJSON *msg_obj = error_obj != NULL ? cJSON_GetObjectItemCaseSensitive(error_obj, "message")
                                       : cJSON_GetObjectItemCaseSensitive(root, "message");
    cJSON *code_obj = error_obj != NULL ? cJSON_GetObjectItemCaseSensitive(error_obj, "code")
                                        : cJSON_GetObjectItemCaseSensitive(root, "code");
    cJSON *rid_obj = error_obj != NULL ? cJSON_GetObjectItemCaseSensitive(error_obj, "request_id")
                                       : cJSON_GetObjectItemCaseSensitive(root, "request_id");

    if (out_message != NULL && message_size > 0) {
        if (cJSON_IsString(msg_obj) && msg_obj->valuestring != NULL) {
            snprintf(out_message, message_size, "%s", msg_obj->valuestring);
        } else {
            out_message[0] = '\0';
        }
    }
    if (out_code != NULL && code_size > 0) {
        if (cJSON_IsString(code_obj) && code_obj->valuestring != NULL) {
            snprintf(out_code, code_size, "%s", code_obj->valuestring);
        } else {
            out_code[0] = '\0';
        }
    }
    if (out_request_id != NULL && request_id_size > 0) {
        if (cJSON_IsString(rid_obj) && rid_obj->valuestring != NULL) {
            snprintf(out_request_id, request_id_size, "%s", rid_obj->valuestring);
        } else {
            out_request_id[0] = '\0';
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

const otool_llm_provider_preset_t otool_llm_provider_ark = {
    .name = "volcengine_ark",
    .default_base_url = "https://ark.cn-beijing.volces.com/api/v3",
    .default_responses_path = "/responses",
    .default_chat_path = "/chat/completions",
    .default_protocol = OTOOL_LLM_PROTOCOL_RESPONSES_SSE,
    .capabilities = OTOOL_LLM_PROVIDER_CAP_RESPONSES |
                    OTOOL_LLM_PROVIDER_CAP_CHAT,
    .request_id_header_name = "x-request-id",
    .build_auth_header = ark_build_auth_header,
    .parse_provider_error = ark_parse_provider_error,
};
