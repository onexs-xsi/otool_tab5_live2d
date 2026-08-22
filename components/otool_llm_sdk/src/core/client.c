/*
 * SPDX-FileCopyrightText: 2026 otool project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "otool_llm_internal.h"
#include "otool_llm_protocol.h"

#include "esp_log.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "otool_llm_client";

#ifndef CONFIG_OTOOL_LLM_MAX_BASE_URL_BYTES
#define CONFIG_OTOOL_LLM_MAX_BASE_URL_BYTES 512
#endif
#ifndef CONFIG_OTOOL_LLM_MAX_ENDPOINT_PATH_BYTES
#define CONFIG_OTOOL_LLM_MAX_ENDPOINT_PATH_BYTES 256
#endif
#ifndef CONFIG_OTOOL_LLM_MAX_API_KEY_BYTES
#define CONFIG_OTOOL_LLM_MAX_API_KEY_BYTES 896
#endif

static bool string_length_at_most(const char *value, size_t maximum)
{
    if (value == NULL) {
        return true;
    }
    size_t length = 0;
    while (length <= maximum && value[length] != '\0') {
        length++;
    }
    return length <= maximum;
}

static bool endpoint_path_valid(const char *path)
{
    return path == NULL ||
           (path[0] == '/' && string_length_at_most(path, CONFIG_OTOOL_LLM_MAX_ENDPOINT_PATH_BYTES));
}

static bool valid_provider(otool_llm_provider_t provider)
{
    return provider == OTOOL_LLM_PROVIDER_OPENAI ||
           provider == OTOOL_LLM_PROVIDER_VOLCENGINE_ARK ||
           provider == OTOOL_LLM_PROVIDER_CUSTOM;
}

static bool valid_protocol(otool_llm_protocol_t protocol)
{
    return protocol == OTOOL_LLM_PROTOCOL_AUTO ||
           protocol == OTOOL_LLM_PROTOCOL_RESPONSES_SSE ||
           protocol == OTOOL_LLM_PROTOCOL_CHAT_COMPLETIONS_SSE;
}

esp_err_t otool_llm_strdup(const char *s, char **out)
{
    *out = NULL;
    if (s == NULL) {
        return ESP_OK;
    }
    size_t len = strlen(s);
    char *copy = (char *)malloc(len + 1);
    if (copy == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, s, len + 1);
    *out = copy;
    return ESP_OK;
}

esp_err_t otool_llm_client_create(const otool_llm_client_config_t *config,
                                  otool_llm_client_handle_t *out_client)
{
    if (out_client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_client = NULL;
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->struct_size < sizeof(otool_llm_client_config_t)) {
        ESP_LOGE(TAG, "client config struct_size %u < %u (older caller?)",
                 (unsigned)config->struct_size, (unsigned)sizeof(otool_llm_client_config_t));
        return ESP_ERR_INVALID_VERSION;
    }
    if (!valid_provider(config->provider) || !valid_protocol(config->protocol)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->provider == OTOOL_LLM_PROVIDER_CUSTOM && config->protocol == OTOOL_LLM_PROTOCOL_AUTO) {
        ESP_LOGE(TAG, "CUSTOM provider requires an explicit protocol (AUTO is not allowed)");
        return ESP_ERR_INVALID_ARG;
    }
    if (config->api_key == NULL || config->api_key[0] == '\0') {
        ESP_LOGE(TAG, "api_key is required and must not come from a compile-time macro");
        return ESP_ERR_INVALID_ARG;
    }
    if (!string_length_at_most(config->api_key, CONFIG_OTOOL_LLM_MAX_API_KEY_BYTES)) {
        ESP_LOGE(TAG, "client credential exceeds its configured boundary");
        return ESP_ERR_INVALID_SIZE;
    }
    if (!endpoint_path_valid(config->responses_path) || !endpoint_path_valid(config->chat_path)) {
        ESP_LOGE(TAG, "endpoint paths must start with '/' and stay within their configured boundary");
        return ESP_ERR_INVALID_ARG;
    }

    const otool_llm_provider_preset_t *provider = otool_llm_provider_get(config->provider);
    if (provider == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const otool_llm_protocol_ops_t *protocol = NULL;
    esp_err_t err = otool_llm_protocol_resolve(config->provider, config->protocol, NULL, &protocol);
    if (err != ESP_OK) {
        return err;
    }

    const char *base_url = config->base_url != NULL ? config->base_url : provider->default_base_url;
    if (base_url == NULL) {
        ESP_LOGE(TAG, "base_url is required for the CUSTOM provider");
        return ESP_ERR_INVALID_ARG;
    }
    if (!string_length_at_most(base_url, CONFIG_OTOOL_LLM_MAX_BASE_URL_BYTES)) {
        ESP_LOGE(TAG, "base_url exceeds its configured boundary");
        return ESP_ERR_INVALID_SIZE;
    }
#ifdef CONFIG_OTOOL_LLM_ALLOW_INSECURE_HTTP
    const bool allow_insecure_http = true;
#else
    const bool allow_insecure_http = false;
#endif
    bool is_https = strncmp(base_url, "https://", 8) == 0 && base_url[8] != '\0';
    bool is_http = strncmp(base_url, "http://", 7) == 0 && base_url[7] != '\0';
    if ((!allow_insecure_http && !is_https) || (allow_insecure_http && !is_https && !is_http)) {
        ESP_LOGE(TAG, "base_url must be https:// unless OTOOL_LLM_ALLOW_INSECURE_HTTP is enabled");
        return ESP_ERR_INVALID_ARG;
    }

    otool_llm_client_handle_t client = (otool_llm_client_handle_t)calloc(1, sizeof(*client));
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    client->provider_id = config->provider;
    client->protocol_id = config->protocol;
    client->provider = provider;
    client->protocol = protocol;
    err = otool_llm_strdup(base_url, &client->base_url);
    if (err == ESP_OK) {
        err = otool_llm_strdup(config->responses_path, &client->responses_path);
    }
    if (err == ESP_OK) {
        err = otool_llm_strdup(config->chat_path, &client->chat_path);
    }
    if (err == ESP_OK) {
        err = otool_llm_strdup(config->api_key, &client->api_key);
    }
    if (err != ESP_OK) {
        if (client->api_key != NULL) {
            otool_llm_secure_zero((void *)client->api_key, strlen(client->api_key));
        }
        free((void *)client->api_key);
        free((void *)client->responses_path);
        free((void *)client->chat_path);
        free((void *)client->base_url);
        free(client);
        return err;
    }

#ifndef CONFIG_OTOOL_LLM_CONNECT_TIMEOUT_MS
#define CONFIG_OTOOL_LLM_CONNECT_TIMEOUT_MS 15000
#endif
#ifndef CONFIG_OTOOL_LLM_READ_TIMEOUT_MS
#define CONFIG_OTOOL_LLM_READ_TIMEOUT_MS 60000
#endif
    client->connect_timeout_ms = config->connect_timeout_ms > 0
                                     ? config->connect_timeout_ms
                                     : CONFIG_OTOOL_LLM_CONNECT_TIMEOUT_MS;
    client->read_timeout_ms = config->read_timeout_ms > 0
                                  ? config->read_timeout_ms
                                  : CONFIG_OTOOL_LLM_READ_TIMEOUT_MS;

    client->lock = xSemaphoreCreateMutex();
    if (client->lock == NULL) {
        otool_llm_secure_zero((void *)client->api_key, strlen(client->api_key));
        free((void *)client->api_key);
        free((void *)client->responses_path);
        free((void *)client->chat_path);
        free((void *)client->base_url);
        free(client);
        return ESP_ERR_NO_MEM;
    }

    *out_client = client;
    return ESP_OK;
}

void otool_llm_client_destroy(otool_llm_client_handle_t client)
{
    if (client == NULL) {
        return;
    }

    bool busy = false;
    if (client->lock != NULL) {
        if (xSemaphoreTake(client->lock, portMAX_DELAY) == pdTRUE) {
            busy = (client->active != NULL) || (client->request_count > 0);
            xSemaphoreGive(client->lock);
        }
    }
    if (busy) {
        ESP_LOGE(TAG, "refusing to destroy client: in-flight request or %u live request handle(s)",
                 (unsigned)client->request_count);
        return;
    }

    if (client->api_key != NULL) {
        otool_llm_secure_zero((void *)client->api_key, strlen(client->api_key));
    }
    free((void *)client->api_key);
    free((void *)client->responses_path);
    free((void *)client->chat_path);
    free((void *)client->base_url);
    if (client->lock != NULL) {
        vSemaphoreDelete(client->lock);
    }
    free(client);
}
