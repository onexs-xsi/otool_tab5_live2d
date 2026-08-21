/*
 * SPDX-FileCopyrightText: 2026 otool project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "otool_llm_provider.h"

extern const otool_llm_provider_preset_t otool_llm_provider_openai;
extern const otool_llm_provider_preset_t otool_llm_provider_ark;
extern const otool_llm_provider_preset_t otool_llm_provider_custom;

const otool_llm_provider_preset_t *otool_llm_provider_get(otool_llm_provider_t id)
{
    switch (id) {
    case OTOOL_LLM_PROVIDER_OPENAI:
        return &otool_llm_provider_openai;
    case OTOOL_LLM_PROVIDER_VOLCENGINE_ARK:
        return &otool_llm_provider_ark;
    case OTOOL_LLM_PROVIDER_CUSTOM:
        return &otool_llm_provider_custom;
    default:
        return NULL;
    }
}
