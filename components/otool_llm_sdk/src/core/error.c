/*
 * SPDX-FileCopyrightText: 2026 otool project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "otool_llm_sdk.h"

const char *otool_llm_err_to_name(esp_err_t err)
{
    switch (err) {
    case OTOOL_LLM_ERR_PROTOCOL:
        return "OTOOL_LLM_ERR_PROTOCOL";
    case OTOOL_LLM_ERR_PROTOCOL_EOF:
        return "OTOOL_LLM_ERR_PROTOCOL_EOF";
    case OTOOL_LLM_ERR_JSON:
        return "OTOOL_LLM_ERR_JSON";
    case OTOOL_LLM_ERR_EVENT_TOO_LARGE:
        return "OTOOL_LLM_ERR_EVENT_TOO_LARGE";
    case OTOOL_LLM_ERR_REQUEST_TOO_LARGE:
        return "OTOOL_LLM_ERR_REQUEST_TOO_LARGE";
    case OTOOL_LLM_ERR_ERROR_BODY_TOO_LARGE:
        return "OTOOL_LLM_ERR_ERROR_BODY_TOO_LARGE";
    case OTOOL_LLM_ERR_PROVIDER:
        return "OTOOL_LLM_ERR_PROVIDER";
    case OTOOL_LLM_ERR_HTTP_STATUS:
        return "OTOOL_LLM_ERR_HTTP_STATUS";
    case OTOOL_LLM_ERR_BAD_CONTENT_TYPE:
        return "OTOOL_LLM_ERR_BAD_CONTENT_TYPE";
    case OTOOL_LLM_ERR_UNSUPPORTED:
        return "OTOOL_LLM_ERR_UNSUPPORTED";
    case OTOOL_LLM_ERR_BUSY:
        return "OTOOL_LLM_ERR_BUSY";
    case OTOOL_LLM_ERR_TERMINATED:
        return "OTOOL_LLM_ERR_TERMINATED";
    default:
        return "OTOOL_LLM_ERR_UNKNOWN";
    }
}
