/*
 * SPDX-FileCopyrightText: 2026 otool project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Local JSON Schema subset validator for tool parameters (plan §9).
 * Supported subset:
 *   - top-level object with "type":"object"
 *   - "properties" (flat object, no nested objects/arrays)
 *   - "required" (string array, names must exist in properties)
 *   - "additionalProperties": false (true is rejected at registration)
 *   - property types: string, number, integer, boolean, null
 *   - "enum" (non-empty array)
 * Anything else is rejected at registration time.
 */

#include "otool_llm_tool_schema.h"

#include "cJSON.h"

#include <ctype.h>
#include <string.h>

#define OTOOL_TOOL_SCHEMA_MAX_DEPTH 3

static bool type_name_supported(const char *type)
{
    return type != NULL &&
           (strcmp(type, "string") == 0 || strcmp(type, "number") == 0 ||
            strcmp(type, "integer") == 0 || strcmp(type, "boolean") == 0 ||
            strcmp(type, "null") == 0);
}

static bool schema_has_unsupported_keywords(cJSON *node)
{
    /* MVP 未实现的关键字：出现即拒绝（计划 §9） */
    static const char *unsupported[] = {
        "minLength", "maxLength", "minimum", "maximum", "pattern",
        "items", "anyOf", "allOf", "oneOf", "not", "format", "$ref",
    };
    for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); i++) {
        if (cJSON_GetObjectItemCaseSensitive(node, unsupported[i]) != NULL) {
            return true;
        }
    }
    return false;
}

static int required_name_count(cJSON *required, const char *property_name)
{
    int found = 0;
    int count = cJSON_GetArraySize(required);
    for (int i = 0; i < count; i++) {
        cJSON *name = cJSON_GetArrayItem(required, i);
        if (cJSON_IsString(name) && name->valuestring != NULL &&
            strcmp(name->valuestring, property_name) == 0) {
            found++;
        }
    }
    return found;
}

static cJSON *parse_exact(const char *json, size_t len)
{
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, len, &end, false);
    if (root == NULL || end == NULL || end > json + len) {
        cJSON_Delete(root);
        return NULL;
    }
    while (end < json + len && isspace((unsigned char)*end)) {
        end++;
    }
    if (end != json + len) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static bool validate_schema_node(cJSON *node, int depth, bool strict)
{
    if (node == NULL || !cJSON_IsObject(node) || depth > OTOOL_TOOL_SCHEMA_MAX_DEPTH) {
        return false;
    }
    if (schema_has_unsupported_keywords(node)) {
        return false;
    }

    cJSON *type = cJSON_GetObjectItemCaseSensitive(node, "type");
    if (!cJSON_IsString(type) || type->valuestring == NULL) {
        return false;
    }

    if (depth == 0) {
        /* 顶层必须是 object */
        if (strcmp(type->valuestring, "object") != 0) {
            return false;
        }
        cJSON *props = cJSON_GetObjectItemCaseSensitive(node, "properties");
        if (props != NULL && !cJSON_IsObject(props)) {
            return false;
        }
        cJSON *ap = cJSON_GetObjectItemCaseSensitive(node, "additionalProperties");
        if ((ap != NULL && !cJSON_IsBool(ap)) || (cJSON_IsBool(ap) && ap->valueint != 0)) {
            return false; /* additionalProperties: true 不支持 */
        }
        if (strict && (ap == NULL || !cJSON_IsFalse(ap))) {
            return false;
        }
        cJSON *req = cJSON_GetObjectItemCaseSensitive(node, "required");
        if (req != NULL) {
            if (!cJSON_IsArray(req)) {
                return false;
            }
            int n = cJSON_GetArraySize(req);
            for (int i = 0; i < n; i++) {
                cJSON *name = cJSON_GetArrayItem(req, i);
                if (!cJSON_IsString(name) || name->valuestring == NULL) {
                    return false;
                }
                /* required 名称必须存在于 properties */
                if (props == NULL ||
                    cJSON_GetObjectItemCaseSensitive(props, name->valuestring) == NULL) {
                    return false;
                }
                if (required_name_count(req, name->valuestring) != 1) {
                    return false;
                }
            }
        }
        if (strict && (props == NULL || req == NULL ||
                       cJSON_GetArraySize(props) != cJSON_GetArraySize(req))) {
            return false;
        }
        /* 每个属性 */
        if (props != NULL) {
            int n = cJSON_GetArraySize(props);
            for (int i = 0; i < n; i++) {
                cJSON *item = cJSON_GetArrayItem(props, i);
                if (item == NULL || item->string == NULL ||
                    (strict && required_name_count(req, item->string) != 1) ||
                    !validate_schema_node(item, depth + 1, strict)) {
                    return false;
                }
            }
        }
        return true;
    }

    /* 属性层：标量类型 + 可选 enum */
    if (!type_name_supported(type->valuestring)) {
        return false;
    }
    cJSON *enum_arr = cJSON_GetObjectItemCaseSensitive(node, "enum");
    if (enum_arr != NULL && (!cJSON_IsArray(enum_arr) || cJSON_GetArraySize(enum_arr) == 0)) {
        return false;
    }
    return true;
}

esp_err_t otool_llm_tool_schema_validate(const char *schema_json, size_t schema_len, bool strict)
{
    if (schema_json == NULL || schema_len == 0) {
        return OTOOL_LLM_ERR_TOOL_SCHEMA;
    }
    cJSON *root = parse_exact(schema_json, schema_len);
    if (root == NULL) {
        return OTOOL_LLM_ERR_TOOL_SCHEMA;
    }
    bool ok = validate_schema_node(root, 0, strict);
    cJSON_Delete(root);
    return ok ? ESP_OK : OTOOL_LLM_ERR_TOOL_SCHEMA;
}

static bool json_type_matches(cJSON *value, const char *type)
{
    if (value == NULL) {
        return false;
    }
    if (strcmp(type, "string") == 0) {
        return cJSON_IsString(value);
    }
    if (strcmp(type, "number") == 0) {
        return cJSON_IsNumber(value);
    }
    if (strcmp(type, "integer") == 0) {
        return cJSON_IsNumber(value) && value->valuedouble == (double)(int64_t)value->valuedouble;
    }
    if (strcmp(type, "boolean") == 0) {
        return cJSON_IsBool(value);
    }
    if (strcmp(type, "null") == 0) {
        return cJSON_IsNull(value);
    }
    return false;
}

static bool enum_contains(cJSON *enum_arr, cJSON *value)
{
    int n = cJSON_GetArraySize(enum_arr);
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(enum_arr, i);
        if (cJSON_Compare(item, value, true)) {
            return true;
        }
    }
    return false;
}

esp_err_t otool_llm_tool_schema_check_arguments(const char *schema_json, size_t schema_len,
                                                const char *arguments_json, size_t args_len)
{
    if (schema_json == NULL || arguments_json == NULL || args_len == 0) {
        return OTOOL_LLM_ERR_TOOL_ARGUMENTS;
    }
    cJSON *schema = parse_exact(schema_json, schema_len);
    if (schema == NULL) {
        return OTOOL_LLM_ERR_TOOL_SCHEMA;
    }
    cJSON *args = parse_exact(arguments_json, args_len);
    if (args == NULL || !cJSON_IsObject(args)) {
        cJSON_Delete(schema);
        cJSON_Delete(args);
        return OTOOL_LLM_ERR_TOOL_ARGUMENTS;
    }

    esp_err_t err = ESP_OK;
    cJSON *props = cJSON_GetObjectItemCaseSensitive(schema, "properties");
    cJSON *required = cJSON_GetObjectItemCaseSensitive(schema, "required");

    /* required 字段必须存在 */
    if (cJSON_IsArray(required)) {
        int n = cJSON_GetArraySize(required);
        for (int i = 0; i < n; i++) {
            cJSON *name = cJSON_GetArrayItem(required, i);
            if (!cJSON_IsString(name) || name->valuestring == NULL ||
                cJSON_GetObjectItemCaseSensitive(args, name->valuestring) == NULL) {
                err = OTOOL_LLM_ERR_TOOL_ARGUMENTS;
                goto out;
            }
        }
    }

    /* 未知字段拒绝 + 类型/enum 校验 */
    int n = cJSON_GetArraySize(args);
    for (int i = 0; i < n; i++) {
        cJSON *arg = cJSON_GetArrayItem(args, i);
        if (arg == NULL || arg->string == NULL) {
            err = OTOOL_LLM_ERR_TOOL_ARGUMENTS;
            goto out;
        }
        cJSON *prop = props != NULL ? cJSON_GetObjectItemCaseSensitive(props, arg->string) : NULL;
        if (prop == NULL) {
            err = OTOOL_LLM_ERR_TOOL_ARGUMENTS; /* 未声明字段 */
            goto out;
        }
        cJSON *ptype = cJSON_GetObjectItemCaseSensitive(prop, "type");
        if (!cJSON_IsString(ptype) || ptype->valuestring == NULL ||
            !json_type_matches(arg, ptype->valuestring)) {
            err = OTOOL_LLM_ERR_TOOL_ARGUMENTS;
            goto out;
        }
        cJSON *enum_arr = cJSON_GetObjectItemCaseSensitive(prop, "enum");
        if (cJSON_IsArray(enum_arr) && !enum_contains(enum_arr, arg)) {
            err = OTOOL_LLM_ERR_TOOL_ARGUMENTS;
            goto out;
        }
    }

out:
    cJSON_Delete(schema);
    cJSON_Delete(args);
    return err;
}
