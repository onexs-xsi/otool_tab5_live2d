#include "otool_speech_sdk.h"
#include "gzip_codec.h"
#include "speech_memory.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "speech.tts";
static const int TTS_MIN_WS_HANDSHAKE_BUFFER_BYTES = 4096;

enum {
    TTS_MSG_FULL_CLIENT = 0x1,
    TTS_MSG_FULL_SERVER = 0x9,
    TTS_MSG_AUDIO_SERVER = 0xb,
    TTS_MSG_ERROR = 0xf,
    TTS_FLAG_WITH_EVENT = 0x4,
    TTS_EVENT_START_CONNECTION = 1,
    TTS_EVENT_FINISH_CONNECTION = 2,
    TTS_EVENT_CONNECTION_STARTED = 50,
    TTS_EVENT_CONNECTION_FAILED = 51,
    TTS_EVENT_CONNECTION_FINISHED = 52,
    TTS_EVENT_START_SESSION = 100,
    TTS_EVENT_FINISH_SESSION = 102,
    TTS_EVENT_SESSION_STARTED = 150,
    TTS_EVENT_SESSION_FINISHED = 152,
    TTS_EVENT_SESSION_FAILED = 153,
    TTS_EVENT_TASK_REQUEST = 200,
    TTS_EVENT_SENTENCE_START = 350,
    TTS_EVENT_SENTENCE_END = 351,
    TTS_EVENT_AUDIO = 352,
};

typedef struct {
    esp_websocket_client_handle_t websocket;
    SemaphoreHandle_t connected_sem;
    SemaphoreHandle_t connection_started_sem;
    SemaphoreHandle_t session_started_sem;
    SemaphoreHandle_t session_finished_sem;
    SemaphoreHandle_t connection_finished_sem;
    SemaphoreHandle_t lock;
    char *headers;
    char *endpoint;
    uint8_t *ws_message;
    size_t ws_message_size;
    size_t ws_frame_base;
    uint8_t *plain_response;
    otool_speech_gzip_decoder_t *gzip_decoder;
    otool_speech_pcm_cb_t on_pcm;
    void *user_ctx;
    esp_err_t terminal_error;
    bool connected;
    bool started;
    bool connection_started;
    bool session_started;
    bool session_finished;
    bool connection_finished;
    bool terminal;
    bool closing;
} tts_session_t;

static void secure_zero(void *data, size_t size)
{
    volatile uint8_t *p = (volatile uint8_t *)data;
    while (size-- != 0) *p++ = 0;
}

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

static void write_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void give_all_waiters(tts_session_t *session)
{
    xSemaphoreGive(session->connected_sem);
    xSemaphoreGive(session->connection_started_sem);
    xSemaphoreGive(session->session_started_sem);
    xSemaphoreGive(session->session_finished_sem);
    xSemaphoreGive(session->connection_finished_sem);
}

static void set_terminal(tts_session_t *session, esp_err_t error)
{
    if (xSemaphoreTake(session->lock, portMAX_DELAY) == pdTRUE) {
        if (!session->terminal) {
            session->terminal = true;
            session->terminal_error = error;
            give_all_waiters(session);
        }
        xSemaphoreGive(session->lock);
    }
}

static esp_err_t provider_json_error(const uint8_t *payload, size_t payload_size,
                                     uint32_t wire_code)
{
    int code = (int)wire_code;
    const char *message = "unknown";
    cJSON *root = cJSON_ParseWithLength((const char *)payload, payload_size);
    if (root != NULL) {
        cJSON *code_item = cJSON_GetObjectItemCaseSensitive(root, "code");
        cJSON *message_item = cJSON_GetObjectItemCaseSensitive(root, "message");
        if (cJSON_IsNumber(code_item)) code = code_item->valueint;
        if (cJSON_IsString(message_item) && message_item->valuestring != NULL) {
            message = message_item->valuestring;
        }
        ESP_LOGE(TAG, "provider error: code=%d message=%.128s", code, message);
        cJSON_Delete(root);
    } else {
        ESP_LOGE(TAG, "provider error: code=%d payload_bytes=%u", code,
                 (unsigned)payload_size);
    }
    return code == 45000030 ? ESP_ERR_NOT_ALLOWED : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t parse_tts_response(tts_session_t *session,
                                    const uint8_t *message, size_t message_size)
{
    if (message_size < 8) return ESP_ERR_INVALID_RESPONSE;
    size_t header_size = (size_t)(message[0] & 0x0f) * 4;
    uint8_t message_type = message[1] >> 4;
    uint8_t flags = message[1] & 0x0f;
    uint8_t serialization = message[2] >> 4;
    uint8_t compression = message[2] & 0x0f;
    if ((message[0] >> 4) != 1 || header_size < 4 || header_size > message_size) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t pos = header_size;
    if (message_type == TTS_MSG_ERROR && flags != TTS_FLAG_WITH_EVENT) {
        if (pos + 8 > message_size) return ESP_ERR_INVALID_RESPONSE;
        uint32_t code = read_be32(message + pos);
        pos += 4;
        size_t payload_size = read_be32(message + pos);
        pos += 4;
        if (payload_size > message_size - pos) return ESP_ERR_INVALID_RESPONSE;
        const uint8_t *payload = message + pos;
        if (compression == 1) {
            esp_err_t err = otool_speech_gzip_decompress(
                session->gzip_decoder, payload, payload_size, session->plain_response,
                CONFIG_OTOOL_SPEECH_TTS_STREAM_BUFFER_BYTES - 1, &payload_size);
            if (err != ESP_OK) return err;
            payload = session->plain_response;
        }
        return provider_json_error(payload, payload_size, code);
    }
    if (flags != TTS_FLAG_WITH_EVENT || pos + 4 > message_size) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    int32_t event = (int32_t)read_be32(message + pos);
    pos += 4;
    if (event != TTS_EVENT_START_CONNECTION && event != TTS_EVENT_FINISH_CONNECTION) {
        if (pos + 4 > message_size) return ESP_ERR_INVALID_RESPONSE;
        size_t id_size = read_be32(message + pos);
        pos += 4;
        if (id_size > message_size - pos) return ESP_ERR_INVALID_RESPONSE;
        pos += id_size;
    }
    if (pos + 4 > message_size) return ESP_ERR_INVALID_RESPONSE;
    size_t payload_size = read_be32(message + pos);
    pos += 4;
    if (payload_size > message_size - pos) return ESP_ERR_INVALID_RESPONSE;

    const uint8_t *payload = message + pos;
    if (compression == 1) {
        esp_err_t err = otool_speech_gzip_decompress(
            session->gzip_decoder, payload, payload_size, session->plain_response,
            CONFIG_OTOOL_SPEECH_TTS_STREAM_BUFFER_BYTES - 1, &payload_size);
        if (err != ESP_OK) return err;
        payload = session->plain_response;
    } else if (compression != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (message_type == TTS_MSG_AUDIO_SERVER) {
        if (event != TTS_EVENT_AUDIO || serialization != 0 || (payload_size & 1U) != 0) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        return session->on_pcm((const int16_t *)payload,
                               payload_size / sizeof(int16_t), session->user_ctx);
    }
    if (message_type != TTS_MSG_FULL_SERVER && message_type != TTS_MSG_ERROR) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (serialization != 1) return ESP_ERR_INVALID_RESPONSE;

    if (event == TTS_EVENT_CONNECTION_FAILED || event == TTS_EVENT_SESSION_FAILED ||
        message_type == TTS_MSG_ERROR) {
        return provider_json_error(payload, payload_size, 0);
    }

    if (xSemaphoreTake(session->lock, portMAX_DELAY) == pdTRUE) {
        switch (event) {
        case TTS_EVENT_CONNECTION_STARTED:
            session->connection_started = true;
            xSemaphoreGive(session->connection_started_sem);
            break;
        case TTS_EVENT_SESSION_STARTED:
            session->session_started = true;
            xSemaphoreGive(session->session_started_sem);
            break;
        case TTS_EVENT_SESSION_FINISHED:
            session->session_finished = true;
            xSemaphoreGive(session->session_finished_sem);
            break;
        case TTS_EVENT_CONNECTION_FINISHED:
            session->connection_finished = true;
            xSemaphoreGive(session->connection_finished_sem);
            break;
        case TTS_EVENT_SENTENCE_START:
        case TTS_EVENT_SENTENCE_END:
            break;
        default:
            ESP_LOGD(TAG, "provider event=%" PRId32 " payload_bytes=%u",
                     event, (unsigned)payload_size);
            break;
        }
        xSemaphoreGive(session->lock);
    }
    return ESP_OK;
}

static void websocket_event_handler(void *handler_arg, esp_event_base_t event_base,
                                    int32_t event_id, void *event_data)
{
    (void)event_base;
    tts_session_t *session = handler_arg;
    esp_websocket_event_data_t *event = event_data;
    switch ((esp_websocket_event_id_t)event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        if (xSemaphoreTake(session->lock, portMAX_DELAY) == pdTRUE) {
            session->connected = true;
            xSemaphoreGive(session->lock);
        }
        xSemaphoreGive(session->connected_sem);
        break;
    case WEBSOCKET_EVENT_DATA: {
        if (event == NULL || event->data_len < 0 || event->payload_offset < 0 ||
            event->payload_len < 0) {
            set_terminal(session, ESP_ERR_INVALID_RESPONSE);
            break;
        }
        if (event->payload_offset == 0) session->ws_frame_base = session->ws_message_size;
        size_t destination = session->ws_frame_base + (size_t)event->payload_offset;
        if (destination > CONFIG_OTOOL_SPEECH_TTS_STREAM_BUFFER_BYTES ||
            (size_t)event->data_len >
                CONFIG_OTOOL_SPEECH_TTS_STREAM_BUFFER_BYTES - destination) {
            set_terminal(session, ESP_ERR_NO_MEM);
            break;
        }
        if (event->data_len > 0) {
            memcpy(session->ws_message + destination, event->data_ptr,
                   (size_t)event->data_len);
        }
        if (event->payload_offset + event->data_len == event->payload_len) {
            session->ws_message_size = session->ws_frame_base + (size_t)event->payload_len;
            if (event->fin) {
                esp_err_t err = parse_tts_response(session, session->ws_message,
                                                   session->ws_message_size);
                session->ws_message_size = 0;
                session->ws_frame_base = 0;
                if (err != ESP_OK) set_terminal(session, err);
            }
        }
        break;
    }
    case WEBSOCKET_EVENT_ERROR:
        if (event != NULL) {
            ESP_LOGE(TAG, "websocket handshake/transport error: http=%d",
                     event->error_handle.esp_ws_handshake_status_code);
        }
        if (!session->closing) set_terminal(session, ESP_FAIL);
        xSemaphoreGive(session->connected_sem);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED: {
        bool unexpected = false;
        if (xSemaphoreTake(session->lock, portMAX_DELAY) == pdTRUE) {
            session->connected = false;
            unexpected = !session->closing && !session->terminal &&
                         !session->session_finished;
            xSemaphoreGive(session->lock);
        }
        if (unexpected) set_terminal(session, ESP_ERR_INVALID_STATE);
        xSemaphoreGive(session->connected_sem);
        break;
    }
    default:
        break;
    }
}

static esp_err_t websocket_send(tts_session_t *session, const uint8_t *data,
                                size_t size, uint32_t timeout_ms)
{
    bool ready = false;
    if (xSemaphoreTake(session->lock, portMAX_DELAY) == pdTRUE) {
        ready = session->connected && !session->terminal && !session->closing;
        xSemaphoreGive(session->lock);
    }
    if (!ready || size > INT_MAX) return ESP_ERR_INVALID_STATE;
    int sent = esp_websocket_client_send_bin(
        session->websocket, (const char *)data, (int)size,
        pdMS_TO_TICKS(timeout_ms != 0 ? timeout_ms : 60000));
    return sent == (int)size ? ESP_OK : ESP_FAIL;
}

static esp_err_t send_event(tts_session_t *session, int32_t event,
                            const char *id, const char *json, uint32_t timeout_ms)
{
    if (json == NULL) json = "{}";
    size_t id_size = id != NULL ? strlen(id) : 0;
    size_t json_size = strlen(json);
    if (id_size > UINT32_MAX || json_size > UINT32_MAX ||
        id_size > SIZE_MAX - json_size - 16) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t frame_size = 4 + 4 + (id != NULL ? 4 + id_size : 0) + 4 + json_size;
    uint8_t *frame = otool_speech_alloc_large(frame_size);
    if (frame == NULL) return ESP_ERR_NO_MEM;
    frame[0] = 0x11;
    frame[1] = (uint8_t)((TTS_MSG_FULL_CLIENT << 4) | TTS_FLAG_WITH_EVENT);
    frame[2] = 0x10;
    frame[3] = 0;
    size_t pos = 4;
    write_be32(frame + pos, (uint32_t)event);
    pos += 4;
    if (id != NULL) {
        write_be32(frame + pos, (uint32_t)id_size);
        pos += 4;
        memcpy(frame + pos, id, id_size);
        pos += id_size;
    }
    write_be32(frame + pos, (uint32_t)json_size);
    pos += 4;
    memcpy(frame + pos, json, json_size);
    esp_err_t err = websocket_send(session, frame, frame_size, timeout_ms);
    secure_zero(frame, frame_size);
    free(frame);
    return err;
}

static char *build_request(const otool_speech_tts_config_t *config,
                           int32_t event, const char *text)
{
    size_t speaker_size = strlen(config->speaker);
    size_t text_size = text != NULL ? strlen(text) : 0;
    if (speaker_size > SIZE_MAX - text_size) return NULL;
    size_t escaped_size = speaker_size + text_size;
    if (escaped_size > ((size_t)INT_MAX - 512) / 6) return NULL;
    size_t request_capacity = escaped_size * 6 + 512;

    cJSON *root = cJSON_CreateObject();
    cJSON *params = cJSON_CreateObject();
    cJSON *audio = cJSON_CreateObject();
    cJSON *speaker = cJSON_CreateStringReference(config->speaker);
    cJSON *text_item = text != NULL ? cJSON_CreateStringReference(text) : NULL;
    bool params_owned = false, audio_owned = false, speaker_owned = false;
    bool text_owned = false;
    bool ok = root != NULL && params != NULL && audio != NULL && speaker != NULL &&
              (text == NULL || text_item != NULL);
    if (ok) {
        params_owned = cJSON_AddItemToObject(root, "req_params", params);
        audio_owned = cJSON_AddItemToObject(params, "audio_params", audio);
        speaker_owned = cJSON_AddItemToObject(params, "speaker", speaker);
        if (text_item != NULL) text_owned = cJSON_AddItemToObject(params, "text", text_item);
        ok = params_owned && audio_owned && speaker_owned &&
             (text_item == NULL || text_owned) &&
             cJSON_AddNumberToObject(root, "event", event) != NULL &&
             cJSON_AddStringToObject(audio, "format", "pcm") != NULL &&
             cJSON_AddNumberToObject(audio, "sample_rate", config->sample_rate_hz) != NULL &&
             cJSON_AddNumberToObject(audio, "speech_rate", config->speech_rate) != NULL &&
             cJSON_AddNumberToObject(audio, "loudness_rate", config->loudness_rate) != NULL;
    }
    char *request = ok ? otool_speech_alloc_large(request_capacity) : NULL;
    if (request != NULL &&
        !cJSON_PrintPreallocated(root, request, (int)request_capacity, false)) {
        free(request);
        request = NULL;
    }
    if (!text_owned) cJSON_Delete(text_item);
    if (!speaker_owned) cJSON_Delete(speaker);
    if (!audio_owned) cJSON_Delete(audio);
    if (!params_owned) cJSON_Delete(params);
    cJSON_Delete(root);
    return request;
}

static esp_err_t wait_phase(tts_session_t *session, SemaphoreHandle_t semaphore,
                            bool *phase_flag, uint32_t timeout_ms)
{
    if (xSemaphoreTake(semaphore,
                       pdMS_TO_TICKS(timeout_ms != 0 ? timeout_ms : 60000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t result = ESP_FAIL;
    if (xSemaphoreTake(session->lock, portMAX_DELAY) == pdTRUE) {
        result = session->terminal ? session->terminal_error
                                   : (*phase_flag ? ESP_OK : ESP_FAIL);
        xSemaphoreGive(session->lock);
    }
    return result;
}

static void destroy_session(tts_session_t *session)
{
    if (session == NULL) return;
    session->closing = true;
    if (session->websocket != NULL) {
        if (session->started) (void)esp_websocket_client_stop(session->websocket);
        (void)esp_websocket_client_destroy(session->websocket);
    }
    if (session->headers != NULL) {
        secure_zero(session->headers, strlen(session->headers));
        free(session->headers);
    }
    free(session->endpoint);
    free(session->ws_message);
    free(session->plain_response);
    otool_speech_gzip_decoder_destroy(session->gzip_decoder);
    if (session->connected_sem != NULL) vSemaphoreDelete(session->connected_sem);
    if (session->connection_started_sem != NULL) vSemaphoreDelete(session->connection_started_sem);
    if (session->session_started_sem != NULL) vSemaphoreDelete(session->session_started_sem);
    if (session->session_finished_sem != NULL) vSemaphoreDelete(session->session_finished_sem);
    if (session->connection_finished_sem != NULL) vSemaphoreDelete(session->connection_finished_sem);
    if (session->lock != NULL) vSemaphoreDelete(session->lock);
    free(session);
}

esp_err_t otool_speech_tts_stream(const otool_speech_tts_config_t *config,
                                  const char *text,
                                  otool_speech_pcm_cb_t on_pcm,
                                  void *user_ctx)
{
    if (config == NULL || config->struct_size < sizeof(otool_speech_tts_config_t) ||
        text == NULL || text[0] == '\0' || on_pcm == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->api_key == NULL || config->api_key[0] == '\0' ||
        config->resource_id == NULL || config->resource_id[0] == '\0' ||
        config->speaker == NULL || config->speaker[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    if (config->timeout_ms > INT_MAX || CONFIG_WS_BUFFER_SIZE < TTS_MIN_WS_HANDSHAKE_BUFFER_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    static const uint32_t valid_rates[] = {8000, 16000, 22050, 24000,
                                           32000, 44100, 48000};
    bool rate_valid = false;
    for (size_t i = 0; i < sizeof(valid_rates) / sizeof(valid_rates[0]); ++i) {
        if (config->sample_rate_hz == valid_rates[i]) rate_valid = true;
    }
    if (!rate_valid || config->speech_rate < -50 || config->speech_rate > 100 ||
        config->loudness_rate < -50 || config->loudness_rate > 100) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t timeout_ms = config->timeout_ms != 0 ? config->timeout_ms : 60000;
    tts_session_t *session = calloc(1, sizeof(*session));
    if (session == NULL) return ESP_ERR_NO_MEM;
    session->connected_sem = xSemaphoreCreateBinary();
    session->connection_started_sem = xSemaphoreCreateBinary();
    session->session_started_sem = xSemaphoreCreateBinary();
    session->session_finished_sem = xSemaphoreCreateBinary();
    session->connection_finished_sem = xSemaphoreCreateBinary();
    session->lock = xSemaphoreCreateMutex();
    session->ws_message =
        otool_speech_alloc_large(CONFIG_OTOOL_SPEECH_TTS_STREAM_BUFFER_BYTES);
    session->plain_response =
        otool_speech_alloc_large(CONFIG_OTOOL_SPEECH_TTS_STREAM_BUFFER_BYTES);
    session->gzip_decoder = otool_speech_gzip_decoder_create();
    session->on_pcm = on_pcm;
    session->user_ctx = user_ctx;
    session->terminal_error = ESP_OK;
    const char *endpoint = config->endpoint != NULL ? config->endpoint
                                                    : OTOOL_SPEECH_TTS_DEFAULT_ENDPOINT;
    session->endpoint = strdup(endpoint);

    char connection_id[37];
    char session_id[37];
    make_uuid(connection_id);
    make_uuid(session_id);
    size_t header_size = strlen(config->api_key) + strlen(config->resource_id) + 176;
    session->headers = malloc(header_size);
    if (session->headers != NULL) session->headers[0] = '\0';
    if (session->connected_sem == NULL || session->connection_started_sem == NULL ||
        session->session_started_sem == NULL || session->session_finished_sem == NULL ||
        session->connection_finished_sem == NULL || session->lock == NULL ||
        session->ws_message == NULL || session->plain_response == NULL ||
        session->gzip_decoder == NULL || session->endpoint == NULL ||
        session->headers == NULL) {
        destroy_session(session);
        return ESP_ERR_NO_MEM;
    }
    snprintf(session->headers, header_size,
             "X-Api-Key: %s\r\nX-Api-Resource-Id: %s\r\nX-Api-Connect-Id: %s\r\n",
             config->api_key, config->resource_id, connection_id);

    esp_websocket_client_config_t ws_config = {
        .uri = session->endpoint,
        .disable_auto_reconnect = true,
        .user_context = session,
        .task_stack = CONFIG_OTOOL_SPEECH_WS_TASK_STACK_SIZE,
        .buffer_size = CONFIG_OTOOL_SPEECH_WS_BUFFER_BYTES,
        .headers = session->headers,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .network_timeout_ms = (int)timeout_ms,
    };
    session->websocket = esp_websocket_client_init(&ws_config);
    if (session->websocket == NULL) {
        destroy_session(session);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_websocket_register_events(
        session->websocket, WEBSOCKET_EVENT_ANY, websocket_event_handler, session);
    if (err == ESP_OK) err = esp_websocket_client_start(session->websocket);
    if (err == ESP_OK) session->started = true;
    if (err == ESP_OK &&
        xSemaphoreTake(session->connected_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        err = ESP_ERR_TIMEOUT;
    }
    if (err == ESP_OK) {
        if (xSemaphoreTake(session->lock, portMAX_DELAY) == pdTRUE) {
            err = session->terminal ? session->terminal_error
                                    : (session->connected ? ESP_OK : ESP_FAIL);
            xSemaphoreGive(session->lock);
        } else {
            err = ESP_FAIL;
        }
    }
    if (err == ESP_OK) err = send_event(session, TTS_EVENT_START_CONNECTION, NULL, "{}", timeout_ms);
    if (err == ESP_OK) {
        err = wait_phase(session, session->connection_started_sem,
                         &session->connection_started, timeout_ms);
    }

    char *start_request = NULL;
    char *task_request = NULL;
    if (err == ESP_OK) {
        start_request = build_request(config, TTS_EVENT_START_SESSION, NULL);
        task_request = build_request(config, TTS_EVENT_TASK_REQUEST, text);
        if (start_request == NULL || task_request == NULL) err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        err = send_event(session, TTS_EVENT_START_SESSION, session_id,
                         start_request, timeout_ms);
    }
    if (err == ESP_OK) {
        err = wait_phase(session, session->session_started_sem,
                         &session->session_started, timeout_ms);
    }
    if (err == ESP_OK) {
        err = send_event(session, TTS_EVENT_TASK_REQUEST, session_id,
                         task_request, timeout_ms);
    }
    if (err == ESP_OK) {
        err = send_event(session, TTS_EVENT_FINISH_SESSION, session_id, "{}", timeout_ms);
    }
    if (err == ESP_OK) {
        err = wait_phase(session, session->session_finished_sem,
                         &session->session_finished, timeout_ms);
    }
    if (err == ESP_OK) {
        esp_err_t finish_err = send_event(session, TTS_EVENT_FINISH_CONNECTION,
                                          NULL, "{}", timeout_ms);
        if (finish_err == ESP_OK) {
            (void)wait_phase(session, session->connection_finished_sem,
                             &session->connection_finished, 3000);
        }
    }

    if (start_request != NULL) {
        secure_zero(start_request, strlen(start_request));
        free(start_request);
    }
    if (task_request != NULL) {
        secure_zero(task_request, strlen(task_request));
        free(task_request);
    }
    destroy_session(session);
    return err;
}
