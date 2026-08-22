/*
 * SPDX-FileCopyrightText: 2026 otool project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tool registry (WP3): deep-copies tool definitions, validates schemas at
 * registration, seal() makes lookups lock-free, destroy requires no users.
 */

#include "otool_llm_tools.h"
#include "otool_llm_tool_schema.h"

#include "esp_log.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "tool_registry";

#ifndef CONFIG_OTOOL_LLM_MAX_TOOLS
#define CONFIG_OTOOL_LLM_MAX_TOOLS 8
#endif
#ifndef CONFIG_OTOOL_LLM_MAX_TOOL_NAME_BYTES
#define CONFIG_OTOOL_LLM_MAX_TOOL_NAME_BYTES 64
#endif
#ifndef CONFIG_OTOOL_LLM_MAX_TOOL_DESCRIPTION_BYTES
#define CONFIG_OTOOL_LLM_MAX_TOOL_DESCRIPTION_BYTES 512
#endif
#ifndef CONFIG_OTOOL_LLM_MAX_TOOL_SCHEMA_BYTES
#define CONFIG_OTOOL_LLM_MAX_TOOL_SCHEMA_BYTES 2048
#endif
#ifndef CONFIG_OTOOL_LLM_MAX_TOTAL_TOOL_SCHEMA_BYTES
#define CONFIG_OTOOL_LLM_MAX_TOTAL_TOOL_SCHEMA_BYTES 8192
#endif
#ifndef CONFIG_OTOOL_LLM_MAX_TOOL_OUTPUT_BYTES
#define CONFIG_OTOOL_LLM_MAX_TOOL_OUTPUT_BYTES 4096
#endif

struct otool_llm_tool_registry {
    otool_llm_tool_definition_t *tools;  /* owned array */
    size_t count;
    size_t capacity;
    size_t schema_bytes;
    bool sealed;
};

static bool tool_name_valid(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    size_t len = strlen(name);
    if (len > CONFIG_OTOOL_LLM_MAX_TOOL_NAME_BYTES) {
        return false;
    }
    /* 安全字符：字母数字下划线（工具名会出现在 JSON 请求中） */
    for (size_t i = 0; i < len; i++) {
        if (!(isalnum((unsigned char)name[i]) || name[i] == '_')) {
            return false;
        }
    }
    return true;
}

esp_err_t otool_llm_tool_registry_create(otool_llm_tool_registry_handle_t *out_reg)
{
    if (out_reg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    otool_llm_tool_registry_handle_t reg =
        (otool_llm_tool_registry_handle_t)calloc(1, sizeof(*reg));
    if (reg == NULL) {
        return ESP_ERR_NO_MEM;
    }
    reg->capacity = CONFIG_OTOOL_LLM_MAX_TOOLS;
    reg->tools = (otool_llm_tool_definition_t *)calloc(reg->capacity, sizeof(*reg->tools));
    if (reg->tools == NULL) {
        free(reg);
        return ESP_ERR_NO_MEM;
    }
    *out_reg = reg;
    return ESP_OK;
}

static void tool_free(otool_llm_tool_definition_t *t)
{
    free((void *)t->name);
    free((void *)t->description);
    free((void *)t->parameters_json_schema);
    memset(t, 0, sizeof(*t));
}

esp_err_t otool_llm_tool_registry_add(otool_llm_tool_registry_handle_t reg,
                                      const otool_llm_tool_definition_t *tool)
{
    if (reg == NULL || tool == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (reg->sealed) {
        return ESP_ERR_INVALID_STATE;
    }
    if (tool->struct_size < sizeof(otool_llm_tool_definition_t)) {
        return ESP_ERR_INVALID_VERSION;
    }
    if (!tool_name_valid(tool->name)) {
        ESP_LOGE(TAG, "tool name '%s' invalid", tool->name ? tool->name : "(null)");
        return ESP_ERR_INVALID_ARG;
    }
    if (tool->parameters_json_schema == NULL) {
        return OTOOL_LLM_ERR_TOOL_SCHEMA;
    }
    if (tool->description != NULL &&
        strlen(tool->description) > CONFIG_OTOOL_LLM_MAX_TOOL_DESCRIPTION_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t schema_len = strlen(tool->parameters_json_schema);
    if (schema_len > CONFIG_OTOOL_LLM_MAX_TOOL_SCHEMA_BYTES ||
        schema_len > CONFIG_OTOOL_LLM_MAX_TOTAL_TOOL_SCHEMA_BYTES - reg->schema_bytes) {
        ESP_LOGE(TAG, "tool '%s' schema exceeds configured byte budget", tool->name);
        return OTOOL_LLM_ERR_TOOL_SCHEMA;
    }
    if (tool->max_output_bytes > CONFIG_OTOOL_LLM_MAX_TOOL_OUTPUT_BYTES) {
        return OTOOL_LLM_ERR_TOOL_OUTPUT_TOO_LARGE;
    }
    /* schema 注册时校验（两层校验第一层） */
    esp_err_t err = otool_llm_tool_schema_validate(tool->parameters_json_schema, schema_len,
                                                   tool->strict);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tool '%s' schema rejected", tool->name);
        return err;
    }
    /* 唯一性 */
    for (size_t i = 0; i < reg->count; i++) {
        if (strcmp(reg->tools[i].name, tool->name) == 0) {
            ESP_LOGE(TAG, "duplicate tool name '%s'", tool->name);
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (reg->count >= reg->capacity) {
        return ESP_ERR_NO_MEM;
    }

    otool_llm_tool_definition_t *dst = &reg->tools[reg->count];
    dst->struct_size = sizeof(*dst);
    dst->strict = tool->strict;
    dst->flags = tool->flags;
    dst->timeout_ms = tool->timeout_ms;
    dst->max_output_bytes = tool->max_output_bytes;
    dst->execute = tool->execute;
    dst->user_ctx = tool->user_ctx;

    char *name = strdup(tool->name);
    char *desc = tool->description != NULL ? strdup(tool->description) : NULL;
    char *schema = strdup(tool->parameters_json_schema);
    if (name == NULL || schema == NULL || (tool->description != NULL && desc == NULL)) {
        free(name);
        free(desc);
        free(schema);
        return ESP_ERR_NO_MEM;
    }
    dst->name = name;
    dst->description = desc;
    dst->parameters_json_schema = schema;
    reg->schema_bytes += schema_len;
    reg->count++;
    return ESP_OK;
}

esp_err_t otool_llm_tool_registry_seal(otool_llm_tool_registry_handle_t reg)
{
    if (reg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    reg->sealed = true;
    return ESP_OK;
}

bool otool_llm_tool_registry_is_sealed(otool_llm_tool_registry_handle_t reg)
{
    return reg != NULL && reg->sealed;
}

void otool_llm_tool_registry_destroy(otool_llm_tool_registry_handle_t reg)
{
    if (reg == NULL) {
        return;
    }
    for (size_t i = 0; i < reg->count; i++) {
        tool_free(&reg->tools[i]);
    }
    free(reg->tools);
    free(reg);
}

const otool_llm_tool_definition_t *otool_llm_tool_registry_find(
    otool_llm_tool_registry_handle_t reg, const char *name)
{
    if (reg == NULL || name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < reg->count; i++) {
        if (strcmp(reg->tools[i].name, name) == 0) {
            return &reg->tools[i];
        }
    }
    return NULL;
}

size_t otool_llm_tool_registry_count(otool_llm_tool_registry_handle_t reg)
{
    return reg != NULL ? reg->count : 0;
}

const otool_llm_tool_definition_t *otool_llm_tool_registry_at(
    otool_llm_tool_registry_handle_t reg, size_t index)
{
    if (reg == NULL || index >= reg->count) {
        return NULL;
    }
    return &reg->tools[index];
}

esp_err_t otool_llm_tool_arguments_validate(const otool_llm_tool_definition_t *tool,
                                            const char *arguments_json, size_t arguments_len)
{
    if (tool == NULL || arguments_json == NULL || arguments_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return otool_llm_tool_schema_check_arguments(tool->parameters_json_schema,
                                                 strlen(tool->parameters_json_schema),
                                                 arguments_json, arguments_len);
}
