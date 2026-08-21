/*
 * SPDX-FileCopyrightText: 2026 otool project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "otool_llm_protocol.h"

esp_err_t otool_llm_protocol_resolve(otool_llm_provider_t provider_id,
                                     otool_llm_protocol_t protocol_id,
                                     const otool_llm_provider_preset_t **out_provider,
                                     const otool_llm_protocol_ops_t **out_ops)
{
    const otool_llm_provider_preset_t *provider = otool_llm_provider_get(provider_id);
    if (provider == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    otool_llm_protocol_t resolved = protocol_id;
    if (resolved == OTOOL_LLM_PROTOCOL_AUTO) {
        if (provider_id == OTOOL_LLM_PROVIDER_CUSTOM) {
            /* Custom has no static default; the caller must pick explicitly. */
            return ESP_ERR_INVALID_ARG;
        }
        resolved = provider->default_protocol;
    }

    const otool_llm_protocol_ops_t *ops = NULL;
    switch (resolved) {
    case OTOOL_LLM_PROTOCOL_RESPONSES_SSE:
        ops = &otool_llm_protocol_responses;
        break;
    case OTOOL_LLM_PROTOCOL_CHAT_COMPLETIONS_SSE:
        ops = &otool_llm_protocol_chat;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t need = (resolved == OTOOL_LLM_PROTOCOL_RESPONSES_SSE)
                        ? OTOOL_LLM_PROVIDER_CAP_RESPONSES
                        : OTOOL_LLM_PROVIDER_CAP_CHAT;
    if (!(provider->capabilities & need)) {
        return OTOOL_LLM_ERR_UNSUPPORTED;
    }

    if (out_provider != NULL) {
        *out_provider = provider;
    }
    if (out_ops != NULL) {
        *out_ops = ops;
    }
    return ESP_OK;
}
