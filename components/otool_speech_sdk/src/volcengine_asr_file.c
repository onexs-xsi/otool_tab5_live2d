#include "otool_speech_sdk.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "speech.asr.file";
static const int32_t PROVIDER_OK = 20000000;
static const int32_t PROVIDER_PROCESSING = 20000001;
static const int32_t PROVIDER_QUEUED = 20000002;
static const int32_t PROVIDER_NOT_GRANTED = 45000030;

typedef struct {
    char *body;
    size_t body_size;
    size_t body_capacity;
    int http_status;
    int32_t provider_code;
    char provider_code_text[24];
    char provider_message[128];
    char log_id[96];
} file_http_response_t;

static void make_uuid(char out[37])
{
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    bytes[6] = (uint8_t)((bytes[6] & 0x0f) | 0x40);
    bytes[8] = (uint8_t)((bytes[8] & 0x3f) | 0x80);
    snprintf(out, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
             bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
}

static void copy_utf8_bounded(char *destination, size_t capacity, const char *source)
{
    if (destination == NULL || capacity == 0) return;
    if (source == NULL) source = "";
    size_t length = strlen(source);
    if (length >= capacity) {
        length = capacity - 1;
        while (length > 0 && (((uint8_t)source[length] & 0xc0U) == 0x80U)) --length;
    }
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static bool supported_audio_format(const char *format)
{
    return format != NULL &&
           (strcmp(format, "raw") == 0 || strcmp(format, "wav") == 0 ||
            strcmp(format, "mp3") == 0 || strcmp(format, "ogg") == 0);
}

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    file_http_response_t *response = event->user_data;
    switch (event->event_id) {
    case HTTP_EVENT_ON_HEADER:
        if (event->header_key == NULL || event->header_value == NULL) break;
        if (strcasecmp(event->header_key, "X-Api-Status-Code") == 0) {
            snprintf(response->provider_code_text, sizeof(response->provider_code_text),
                     "%.23s", event->header_value);
        } else if (strcasecmp(event->header_key, "X-Api-Message") == 0) {
            snprintf(response->provider_message, sizeof(response->provider_message),
                     "%.127s", event->header_value);
        } else if (strcasecmp(event->header_key, "X-Tt-Logid") == 0) {
            snprintf(response->log_id, sizeof(response->log_id), "%.95s", event->header_value);
        }
        break;
    case HTTP_EVENT_ON_DATA:
        if (event->data_len < 0 || response->body_size > response->body_capacity ||
            (size_t)event->data_len > response->body_capacity - response->body_size) {
            return ESP_ERR_NO_MEM;
        }
        if (event->data_len > 0) {
            memcpy(response->body + response->body_size, event->data, (size_t)event->data_len);
            response->body_size += (size_t)event->data_len;
            response->body[response->body_size] = '\0';
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        response->http_status = esp_http_client_get_status_code(event->client);
        break;
    default:
        break;
    }
    return ESP_OK;
}

static void parse_provider_fallback(file_http_response_t *response)
{
    if (response->provider_code_text[0] != '\0') {
        char *end = NULL;
        errno = 0;
        long parsed = strtol(response->provider_code_text, &end, 10);
        if (errno == 0 && end != response->provider_code_text && *end == '\0' &&
            parsed >= INT32_MIN && parsed <= INT32_MAX) {
            response->provider_code = (int32_t)parsed;
        }
    }
    if (response->body_size == 0 ||
        (response->provider_code != 0 && response->provider_message[0] != '\0')) return;

    cJSON *root = cJSON_ParseWithLength(response->body, response->body_size);
    if (root == NULL) return;
    cJSON *envelope = root;
    cJSON *header = cJSON_GetObjectItemCaseSensitive(root, "header");
    if (cJSON_IsObject(header)) envelope = header;
    cJSON *code = cJSON_GetObjectItemCaseSensitive(envelope, "code");
    cJSON *message = cJSON_GetObjectItemCaseSensitive(envelope, "message");
    if (response->provider_code == 0 && cJSON_IsNumber(code)) {
        response->provider_code = (int32_t)code->valuedouble;
    }
    if (response->provider_message[0] == '\0' && cJSON_IsString(message)) {
        snprintf(response->provider_message, sizeof(response->provider_message),
                 "%.127s", message->valuestring);
    }
    cJSON_Delete(root);
}

static esp_err_t post_json(const otool_speech_asr_file_config_t *config,
                           const char *endpoint, const char *request_id,
                           const char *json, bool submit,
                           file_http_response_t *response)
{
    size_t json_size = strlen(json);
    if (json_size > (size_t)INT_MAX - 256U || config->request_timeout_ms > INT_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    memset(response, 0, sizeof(*response));
    response->body_capacity = CONFIG_OTOOL_SPEECH_ASR_FILE_MAX_RESPONSE_BYTES;
    response->body = calloc(1, response->body_capacity + 1U);
    if (response->body == NULL) return ESP_ERR_NO_MEM;

    esp_http_client_config_t http_config = {
        .url = endpoint,
        .event_handler = http_event_handler,
        .user_data = response,
        .timeout_ms = (int)(config->request_timeout_ms != 0
                            ? config->request_timeout_ms : 15000),
        .buffer_size = 2048,
        .buffer_size_tx = (int)json_size + 256,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (client == NULL) {
        free(response->body);
        response->body = NULL;
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "X-Api-Key", config->api_key);
    esp_http_client_set_header(client, "X-Api-Resource-Id", config->resource_id);
    esp_http_client_set_header(client, "X-Api-Request-Id", request_id);
    if (submit) esp_http_client_set_header(client, "X-Api-Sequence", "-1");
    esp_http_client_set_post_field(client, json, (int)json_size);

    esp_err_t err = esp_http_client_perform(client);
    response->http_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) return err;
    parse_provider_fallback(response);
    if (response->provider_code == PROVIDER_NOT_GRANTED) return ESP_ERR_NOT_ALLOWED;
    if (response->http_status < 200 || response->http_status >= 300) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static void free_http_response(file_http_response_t *response)
{
    free(response->body);
    response->body = NULL;
    response->body_size = 0;
}

static char *build_submit_request(const otool_speech_asr_file_config_t *config,
                                  const char *audio_url, const char *audio_format)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *user = cJSON_CreateObject();
    cJSON *audio = cJSON_CreateObject();
    cJSON *request = cJSON_CreateObject();
    bool user_owned = false, audio_owned = false, request_owned = false;
    bool ok = root != NULL && user != NULL && audio != NULL && request != NULL;
    if (ok) {
        user_owned = cJSON_AddItemToObject(root, "user", user);
        audio_owned = cJSON_AddItemToObject(root, "audio", audio);
        request_owned = cJSON_AddItemToObject(root, "request", request);
        ok = user_owned && audio_owned && request_owned &&
             cJSON_AddStringToObject(user, "uid", "otool-tab5") != NULL &&
             cJSON_AddStringToObject(audio, "url", audio_url) != NULL &&
             cJSON_AddStringToObject(audio, "format", audio_format) != NULL &&
             cJSON_AddStringToObject(request, "model_name", "bigmodel") != NULL &&
             cJSON_AddBoolToObject(request, "enable_itn", config->enable_itn) != NULL &&
             cJSON_AddBoolToObject(request, "enable_punc", config->enable_punctuation) != NULL &&
             cJSON_AddBoolToObject(request, "show_utterances", config->show_utterances) != NULL;
        if (ok && config->language != NULL && config->language[0] != '\0') {
            ok = cJSON_AddStringToObject(audio, "language", config->language) != NULL;
        }
    }
    char *json = ok ? cJSON_PrintUnformatted(root) : NULL;
    if (!user_owned) cJSON_Delete(user);
    if (!audio_owned) cJSON_Delete(audio);
    if (!request_owned) cJSON_Delete(request);
    cJSON_Delete(root);
    return json;
}

static void update_result(otool_speech_asr_file_result_t *result,
                          const file_http_response_t *response,
                          const char *request_id)
{
    if (result == NULL) return;
    result->provider_code = response->provider_code;
    copy_utf8_bounded(result->request_id, sizeof(result->request_id), request_id);
    copy_utf8_bounded(result->log_id, sizeof(result->log_id), response->log_id);
    copy_utf8_bounded(result->message, sizeof(result->message), response->provider_message);
}

static esp_err_t parse_query_result(file_http_response_t *response,
                                    char *transcript, size_t transcript_capacity,
                                    otool_speech_asr_file_result_t *result)
{
    if (response->provider_code == PROVIDER_PROCESSING ||
        response->provider_code == PROVIDER_QUEUED) {
        return ESP_ERR_NOT_FINISHED;
    }
    if (response->provider_code != PROVIDER_OK) return ESP_ERR_INVALID_RESPONSE;
    cJSON *root = cJSON_ParseWithLength(response->body, response->body_size);
    if (root == NULL) return ESP_ERR_INVALID_RESPONSE;
    cJSON *result_json = cJSON_GetObjectItemCaseSensitive(root, "result");
    cJSON *text = cJSON_IsObject(result_json)
        ? cJSON_GetObjectItemCaseSensitive(result_json, "text") : NULL;
    cJSON *audio_info = cJSON_GetObjectItemCaseSensitive(root, "audio_info");
    cJSON *duration = cJSON_IsObject(audio_info)
        ? cJSON_GetObjectItemCaseSensitive(audio_info, "duration") : NULL;
    if (result != NULL && cJSON_IsNumber(duration) && duration->valuedouble >= 0) {
        result->audio_duration_ms = (uint32_t)duration->valuedouble;
    }
    esp_err_t err = ESP_ERR_INVALID_RESPONSE;
    if (cJSON_IsString(text) && text->valuestring != NULL) {
        copy_utf8_bounded(transcript, transcript_capacity, text->valuestring);
        err = ESP_OK;
    }
    cJSON_Delete(root);
    return err;
}

esp_err_t otool_speech_asr_file_recognize_url(
    const otool_speech_asr_file_config_t *config,
    const char *audio_url,
    const char *audio_format,
    char *transcript,
    size_t transcript_capacity,
    otool_speech_asr_file_result_t *result)
{
    if (config == NULL || config->struct_size < sizeof(*config) ||
        audio_url == NULL || transcript == NULL || transcript_capacity == 0 ||
        !supported_audio_format(audio_format)) {
        return ESP_ERR_INVALID_ARG;
    }
    transcript[0] = '\0';
    if (result != NULL) memset(result, 0, sizeof(*result));
    if (config->api_key == NULL || config->api_key[0] == '\0' ||
        config->resource_id == NULL || config->resource_id[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    if ((strncmp(audio_url, "https://", 8) != 0 &&
         strncmp(audio_url, "http://", 7) != 0) ||
        config->query_interval_ms > INT_MAX || config->overall_timeout_ms > INT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *submit_endpoint = config->submit_endpoint != NULL
        ? config->submit_endpoint : OTOOL_SPEECH_ASR_FILE_SUBMIT_DEFAULT_ENDPOINT;
    const char *query_endpoint = config->query_endpoint != NULL
        ? config->query_endpoint : OTOOL_SPEECH_ASR_FILE_QUERY_DEFAULT_ENDPOINT;
    char request_id[37];
    make_uuid(request_id);
    char *submit_json = build_submit_request(config, audio_url, audio_format);
    if (submit_json == NULL) return ESP_ERR_NO_MEM;

    file_http_response_t response = {0};
    esp_err_t err = post_json(config, submit_endpoint, request_id, submit_json, true, &response);
    memset(submit_json, 0, strlen(submit_json));
    cJSON_free(submit_json);
    update_result(result, &response, request_id);
    if (err == ESP_OK && response.provider_code != PROVIDER_OK) {
        err = response.provider_code == PROVIDER_NOT_GRANTED
            ? ESP_ERR_NOT_ALLOWED : ESP_ERR_INVALID_RESPONSE;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "submit failed: http=%d provider=%" PRId32 " message=%.96s logid=%s",
                 response.http_status, response.provider_code,
                 response.provider_message[0] != '\0' ? response.provider_message : "-",
                 response.log_id[0] != '\0' ? response.log_id : "-");
        free_http_response(&response);
        return err;
    }
    free_http_response(&response);

    uint32_t interval_ms = config->query_interval_ms != 0 ? config->query_interval_ms : 500;
    uint32_t overall_ms = config->overall_timeout_ms != 0 ? config->overall_timeout_ms : 120000;
    int64_t deadline_us = esp_timer_get_time() + (int64_t)overall_ms * 1000;
    do {
        vTaskDelay(pdMS_TO_TICKS(interval_ms));
        err = post_json(config, query_endpoint, request_id, "{}", false, &response);
        update_result(result, &response, request_id);
        if (err == ESP_OK) err = parse_query_result(&response, transcript, transcript_capacity, result);
        if (err == ESP_OK) {
            free_http_response(&response);
            return ESP_OK;
        }
        bool pending = err == ESP_ERR_NOT_FINISHED;
        if (!pending) {
            ESP_LOGE(TAG, "query failed: http=%d provider=%" PRId32 " message=%.96s logid=%s",
                     response.http_status, response.provider_code,
                     response.provider_message[0] != '\0' ? response.provider_message : "-",
                     response.log_id[0] != '\0' ? response.log_id : "-");
        }
        free_http_response(&response);
        if (!pending) return err;
    } while (esp_timer_get_time() < deadline_us);

    return ESP_ERR_TIMEOUT;
}
