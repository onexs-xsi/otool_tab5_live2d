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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <limits.h>

static const char *TAG = "speech.asr";
static const uint32_t ASR_SAMPLE_RATE_HZ = 16000;
static const int ASR_MIN_WS_HANDSHAKE_BUFFER_BYTES = 4096;

struct otool_speech_asr_session {
    esp_websocket_client_handle_t websocket;
    SemaphoreHandle_t connected_sem;
    SemaphoreHandle_t done_sem;
    SemaphoreHandle_t lock;
    otool_speech_transcript_cb_t on_transcript;
    void *user_ctx;
    char *headers;
    char *endpoint;
    uint8_t *audio_packet;
    size_t audio_packet_capacity;
    size_t audio_packet_size;
    uint8_t *tx_buffer;
    size_t tx_capacity;
    uint8_t *ws_message;
    size_t ws_message_size;
    size_t ws_frame_base;
    uint8_t *plain_response;
    char *final_text;
    otool_speech_gzip_encoder_t *gzip_encoder;
    otool_speech_gzip_decoder_t *gzip_decoder;
    esp_err_t terminal_error;
    bool connected;
    bool started;
    bool terminal;
    bool closing;
};

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

static void copy_utf8_bounded(char *destination, size_t capacity, const char *source)
{
    if (capacity == 0) return;
    size_t length = strlen(source);
    if (length >= capacity) {
        length = capacity - 1;
        while (length > 0 && (((uint8_t)source[length] & 0xc0) == 0x80)) --length;
    }
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static void set_terminal(struct otool_speech_asr_session *session, esp_err_t error)
{
    if (xSemaphoreTake(session->lock, portMAX_DELAY) == pdTRUE) {
        if (!session->terminal) {
            session->terminal = true;
            session->terminal_error = error;
            xSemaphoreGive(session->done_sem);
        }
        xSemaphoreGive(session->lock);
    }
}

static esp_err_t parse_asr_response(struct otool_speech_asr_session *session,
                                    const uint8_t *message, size_t message_size)
{
    if (message_size < 8) return ESP_ERR_INVALID_RESPONSE;
    size_t header_size = (size_t)(message[0] & 0x0f) * 4;
    uint8_t message_type = message[1] >> 4;
    uint8_t flags = message[1] & 0x0f;
    uint8_t serialization = message[2] >> 4;
    uint8_t compression = message[2] & 0x0f;
    if (header_size < 4 || header_size > message_size) return ESP_ERR_INVALID_RESPONSE;

    size_t pos = header_size;
    uint32_t provider_error = 0;
    if (message_type == 0x9 && (flags == 0x1 || flags == 0x3)) {
        if (pos + 4 > message_size) return ESP_ERR_INVALID_RESPONSE;
        pos += 4; /* sequence */
    } else if (message_type == 0xf) {
        if (pos + 4 > message_size) return ESP_ERR_INVALID_RESPONSE;
        provider_error = read_be32(message + pos);
        pos += 4;
    } else if (message_type != 0x9) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (pos + 4 > message_size) return ESP_ERR_INVALID_RESPONSE;
    size_t payload_size = read_be32(message + pos);
    pos += 4;
    if (payload_size > message_size - pos) return ESP_ERR_INVALID_RESPONSE;

    const uint8_t *json_data = message + pos;
    size_t json_size = payload_size;
    if (compression == 1) {
        esp_err_t err = otool_speech_gzip_decompress(session->gzip_decoder,
                                                     json_data, json_size,
                                                     session->plain_response,
                                                     CONFIG_OTOOL_SPEECH_ASR_MAX_RESPONSE_BYTES - 1,
                                                     &json_size);
        if (err != ESP_OK) return err;
        session->plain_response[json_size] = '\0';
        json_data = session->plain_response;
    } else if (compression != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (serialization != 1 || json_size == 0) return ESP_ERR_INVALID_RESPONSE;

    cJSON *root = cJSON_ParseWithLength((const char *)json_data, json_size);
    if (root == NULL) return ESP_ERR_INVALID_RESPONSE;
    if (message_type == 0xf) {
        cJSON *message_item = cJSON_GetObjectItemCaseSensitive(root, "message");
        ESP_LOGE(TAG, "provider error: code=%" PRIu32 " message=%.96s", provider_error,
                 cJSON_IsString(message_item) ? message_item->valuestring : "unknown");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (cJSON_IsArray(result)) result = cJSON_GetArrayItem(result, 0);
    cJSON *text = cJSON_IsObject(result)
        ? cJSON_GetObjectItemCaseSensitive(result, "text") : NULL;
    bool final_packet = flags == 0x2 || flags == 0x3;
    if (cJSON_IsString(text) && text->valuestring != NULL) {
        if (xSemaphoreTake(session->lock, portMAX_DELAY) == pdTRUE) {
            copy_utf8_bounded(session->final_text,
                              CONFIG_OTOOL_SPEECH_ASR_MAX_TRANSCRIPT_BYTES,
                              text->valuestring);
            xSemaphoreGive(session->lock);
        }
        if (session->on_transcript != NULL) {
            session->on_transcript(text->valuestring, final_packet, session->user_ctx);
        }
    }
    cJSON_Delete(root);
    if (final_packet) set_terminal(session, ESP_OK);
    return ESP_OK;
}

static void websocket_event_handler(void *handler_arg, esp_event_base_t event_base,
                                    int32_t event_id, void *event_data)
{
    (void)event_base;
    struct otool_speech_asr_session *session = handler_arg;
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
        if (event->payload_offset == 0) {
            session->ws_frame_base = session->ws_message_size;
        }
        size_t destination = session->ws_frame_base + (size_t)event->payload_offset;
        if (destination > CONFIG_OTOOL_SPEECH_ASR_MAX_RESPONSE_BYTES ||
            (size_t)event->data_len > CONFIG_OTOOL_SPEECH_ASR_MAX_RESPONSE_BYTES - destination) {
            set_terminal(session, ESP_ERR_NO_MEM);
            break;
        }
        if (event->data_len > 0) {
            memcpy(session->ws_message + destination, event->data_ptr, (size_t)event->data_len);
        }
        if (event->payload_offset + event->data_len == event->payload_len) {
            session->ws_message_size = session->ws_frame_base + (size_t)event->payload_len;
            if (event->fin) {
                esp_err_t err = parse_asr_response(session, session->ws_message,
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
            /* Some managed-component releases don't initialize every field in
             * error_handle. The HTTP status is populated for Upgrade failures
             * and is the useful provider-facing diagnostic here. */
            ESP_LOGE(TAG, "websocket handshake/transport error: http=%d",
                     event->error_handle.esp_ws_handshake_status_code);
        }
        if (!session->closing) set_terminal(session, ESP_FAIL);
        /* Publish the terminal state before waking asr_open(). Otherwise the
         * failed handshake can be observed as a successful connection. */
        xSemaphoreGive(session->connected_sem);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED: {
        bool should_set_terminal = false;
        if (xSemaphoreTake(session->lock, portMAX_DELAY) == pdTRUE) {
            session->connected = false;
            should_set_terminal = !session->closing && !session->terminal;
            xSemaphoreGive(session->lock);
        }
        if (should_set_terminal) set_terminal(session, ESP_ERR_INVALID_STATE);
        xSemaphoreGive(session->connected_sem);
        break;
    }
    default:
        break;
    }
}

static esp_err_t websocket_send(struct otool_speech_asr_session *session,
                                const uint8_t *data, size_t size)
{
    bool connected = false;
    if (xSemaphoreTake(session->lock, portMAX_DELAY) == pdTRUE) {
        connected = session->connected && !session->terminal && !session->closing;
        xSemaphoreGive(session->lock);
    }
    if (!connected || size > INT_MAX) return ESP_ERR_INVALID_STATE;
    int sent = esp_websocket_client_send_bin(session->websocket, (const char *)data,
                                             (int)size, pdMS_TO_TICKS(10000));
    return sent == (int)size ? ESP_OK : ESP_FAIL;
}

static esp_err_t send_initial_request(struct otool_speech_asr_session *session,
                                      const otool_speech_asr_config_t *config)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *audio = cJSON_CreateObject();
    cJSON *request = cJSON_CreateObject();
    bool owned_audio = false, owned_request = false;
    bool ok = root != NULL && audio != NULL && request != NULL;
    if (ok) {
        owned_audio = cJSON_AddItemToObject(root, "audio", audio);
        owned_request = cJSON_AddItemToObject(root, "request", request);
        ok = owned_audio && owned_request &&
             cJSON_AddStringToObject(audio, "format", "pcm") != NULL &&
             cJSON_AddStringToObject(audio, "codec", "raw") != NULL &&
             cJSON_AddNumberToObject(audio, "rate", ASR_SAMPLE_RATE_HZ) != NULL &&
             cJSON_AddNumberToObject(audio, "bits", 16) != NULL &&
             cJSON_AddNumberToObject(audio, "channel", 1) != NULL &&
             cJSON_AddStringToObject(request, "model_name", "bigmodel") != NULL &&
             cJSON_AddBoolToObject(request, "enable_itn", config->enable_itn) != NULL &&
             cJSON_AddBoolToObject(request, "enable_punc", config->enable_punctuation) != NULL &&
             cJSON_AddBoolToObject(request, "enable_ddc", config->enable_ddc) != NULL &&
             cJSON_AddBoolToObject(request, "enable_nonstream", config->enable_nonstream) != NULL &&
             cJSON_AddBoolToObject(request, "show_utterances", true) != NULL;
    }
    char *json = ok ? cJSON_PrintUnformatted(root) : NULL;
    if (!owned_audio) cJSON_Delete(audio);
    if (!owned_request) cJSON_Delete(request);
    cJSON_Delete(root);
    if (json == NULL) {
        ESP_LOGE(TAG, "initial request JSON allocation failed");
        return ESP_ERR_NO_MEM;
    }

    size_t compressed_size = 0;
    size_t json_size = strlen(json);
    esp_err_t err = otool_speech_gzip_compress(session->gzip_encoder,
                                               (const uint8_t *)json, json_size,
                                               session->tx_buffer + 8,
                                               session->tx_capacity - 8,
                                               &compressed_size);
    secure_zero(json, json_size);
    cJSON_free(json);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "initial request compression failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    session->tx_buffer[0] = 0x11;
    session->tx_buffer[1] = 0x10;
    session->tx_buffer[2] = 0x11;
    session->tx_buffer[3] = 0x00;
    write_be32(session->tx_buffer + 4, (uint32_t)compressed_size);
    return websocket_send(session, session->tx_buffer, compressed_size + 8);
}

static esp_err_t send_audio_packet(struct otool_speech_asr_session *session, bool final_packet)
{
    size_t compressed_size = 0;
    esp_err_t err = otool_speech_gzip_compress(session->gzip_encoder,
                                               session->audio_packet,
                                               session->audio_packet_size,
                                               session->tx_buffer + 8,
                                               session->tx_capacity - 8,
                                               &compressed_size);
    if (err != ESP_OK) return err;
    session->tx_buffer[0] = 0x11;
    session->tx_buffer[1] = (uint8_t)(0x20 | (final_packet ? 0x02 : 0x00));
    session->tx_buffer[2] = 0x01;
    session->tx_buffer[3] = 0x00;
    write_be32(session->tx_buffer + 4, (uint32_t)compressed_size);
    err = websocket_send(session, session->tx_buffer, compressed_size + 8);
    if (err == ESP_OK) session->audio_packet_size = 0;
    return err;
}

esp_err_t otool_speech_asr_open(const otool_speech_asr_config_t *config,
                                otool_speech_asr_handle_t *out_session)
{
    if (config == NULL || out_session == NULL ||
        config->struct_size < sizeof(otool_speech_asr_config_t)) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_session = NULL;
    if (config->api_key == NULL || config->api_key[0] == '\0' ||
        config->resource_id == NULL || config->resource_id[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    if (config->connect_timeout_ms > INT_MAX) return ESP_ERR_INVALID_ARG;
    if (CONFIG_WS_BUFFER_SIZE < ASR_MIN_WS_HANDSHAKE_BUFFER_BYTES) {
        ESP_LOGE(TAG,
                 "CONFIG_WS_BUFFER_SIZE=%d is too small for the provider HTTP "
                 "Upgrade response; configure at least %d bytes",
                 CONFIG_WS_BUFFER_SIZE, ASR_MIN_WS_HANDSHAKE_BUFFER_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }

    struct otool_speech_asr_session *session = calloc(1, sizeof(*session));
    if (session == NULL) return ESP_ERR_NO_MEM;
    session->connected_sem = xSemaphoreCreateBinary();
    session->done_sem = xSemaphoreCreateBinary();
    session->lock = xSemaphoreCreateMutex();
    session->on_transcript = config->on_transcript;
    session->user_ctx = config->user_ctx;
    session->terminal_error = ESP_OK;
    session->audio_packet_capacity =
        ASR_SAMPLE_RATE_HZ * CONFIG_OTOOL_SPEECH_ASR_PACKET_MS / 1000 * sizeof(int16_t);
    session->tx_capacity = otool_speech_gzip_bound(session->audio_packet_capacity) + 8;
    session->audio_packet = otool_speech_alloc_large(session->audio_packet_capacity);
    session->tx_buffer = otool_speech_alloc_large(session->tx_capacity);
    session->ws_message =
        otool_speech_alloc_large(CONFIG_OTOOL_SPEECH_ASR_MAX_RESPONSE_BYTES);
    session->plain_response =
        otool_speech_alloc_large(CONFIG_OTOOL_SPEECH_ASR_MAX_RESPONSE_BYTES);
    session->final_text = calloc(1, CONFIG_OTOOL_SPEECH_ASR_MAX_TRANSCRIPT_BYTES);
    session->gzip_encoder = otool_speech_gzip_encoder_create();
    session->gzip_decoder = otool_speech_gzip_decoder_create();
    const char *endpoint = config->endpoint != NULL ? config->endpoint : OTOOL_SPEECH_ASR_DEFAULT_ENDPOINT;
    session->endpoint = strdup(endpoint);

    char request_id[37];
    make_uuid(request_id);
    size_t header_size = strlen(config->api_key) + strlen(config->resource_id) + 192;
    session->headers = malloc(header_size);
    if (session->headers != NULL) session->headers[0] = '\0';
    if (session->connected_sem == NULL || session->done_sem == NULL || session->lock == NULL ||
        session->audio_packet == NULL || session->tx_buffer == NULL ||
        session->ws_message == NULL || session->plain_response == NULL ||
        session->final_text == NULL || session->gzip_encoder == NULL ||
        session->gzip_decoder == NULL || session->endpoint == NULL ||
        session->headers == NULL) {
        otool_speech_asr_close(session);
        return ESP_ERR_NO_MEM;
    }
    snprintf(session->headers, header_size,
             "X-Api-Key: %s\r\nX-Api-Resource-Id: %s\r\n"
             "X-Api-Request-Id: %s\r\nX-Api-Sequence: -1\r\n",
             config->api_key, config->resource_id, request_id);

    esp_websocket_client_config_t ws_config = {
        .uri = session->endpoint,
        .disable_auto_reconnect = true,
        .user_context = session,
        .task_stack = CONFIG_OTOOL_SPEECH_WS_TASK_STACK_SIZE,
        .buffer_size = CONFIG_OTOOL_SPEECH_WS_BUFFER_BYTES,
        .headers = session->headers,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .network_timeout_ms = (int)(config->connect_timeout_ms != 0
                                    ? config->connect_timeout_ms : 15000),
    };
    session->websocket = esp_websocket_client_init(&ws_config);
    if (session->websocket == NULL) {
        otool_speech_asr_close(session);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_websocket_register_events(session->websocket, WEBSOCKET_EVENT_ANY,
                                                   websocket_event_handler, session);
    if (err == ESP_OK) err = esp_websocket_client_start(session->websocket);
    if (err != ESP_OK) {
        otool_speech_asr_close(session);
        return err;
    }
    session->started = true;
    TickType_t wait_ticks = pdMS_TO_TICKS(config->connect_timeout_ms != 0
                                          ? config->connect_timeout_ms : 15000);
    if (xSemaphoreTake(session->connected_sem, wait_ticks) != pdTRUE) {
        otool_speech_asr_close(session);
        return ESP_ERR_TIMEOUT;
    }
    bool connected = false;
    bool terminal = false;
    esp_err_t connect_error = ESP_FAIL;
    if (xSemaphoreTake(session->lock, portMAX_DELAY) == pdTRUE) {
        connected = session->connected;
        terminal = session->terminal;
        connect_error = terminal ? session->terminal_error : ESP_FAIL;
        xSemaphoreGive(session->lock);
    }
    if (!connected || terminal) {
        otool_speech_asr_close(session);
        return connect_error;
    }
    err = send_initial_request(session, config);
    if (err != ESP_OK) {
        otool_speech_asr_close(session);
        return err;
    }
    ESP_LOGI(TAG, "stream ready: 16 kHz mono PCM, %d ms packets",
             CONFIG_OTOOL_SPEECH_ASR_PACKET_MS);
    *out_session = session;
    return ESP_OK;
}

esp_err_t otool_speech_asr_write_pcm(otool_speech_asr_handle_t session,
                                     const int16_t *samples, size_t sample_count)
{
    if (session == NULL || (samples == NULL && sample_count != 0)) return ESP_ERR_INVALID_ARG;
    if (sample_count > SIZE_MAX / sizeof(int16_t)) return ESP_ERR_INVALID_SIZE;
    if (session->terminal || session->closing) return ESP_ERR_INVALID_STATE;
    const uint8_t *input = (const uint8_t *)samples;
    size_t bytes = sample_count * sizeof(int16_t);
    while (bytes != 0) {
        size_t room = session->audio_packet_capacity - session->audio_packet_size;
        size_t take = bytes < room ? bytes : room;
        memcpy(session->audio_packet + session->audio_packet_size, input, take);
        session->audio_packet_size += take;
        input += take;
        bytes -= take;
        if (session->audio_packet_size == session->audio_packet_capacity) {
            esp_err_t err = send_audio_packet(session, false);
            if (err != ESP_OK) {
                set_terminal(session, err);
                return err;
            }
        }
    }
    return ESP_OK;
}

esp_err_t otool_speech_asr_finish(otool_speech_asr_handle_t session,
                                  char *transcript, size_t transcript_capacity,
                                  uint32_t timeout_ms)
{
    if (session == NULL || transcript == NULL || transcript_capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    transcript[0] = '\0';
    if (!session->terminal) {
        esp_err_t err = send_audio_packet(session, true);
        if (err != ESP_OK) set_terminal(session, err);
    }
    if (!session->terminal &&
        xSemaphoreTake(session->done_sem, pdMS_TO_TICKS(timeout_ms != 0 ? timeout_ms : 15000)) != pdTRUE) {
        set_terminal(session, ESP_ERR_TIMEOUT);
    }
    if (xSemaphoreTake(session->lock, portMAX_DELAY) == pdTRUE) {
        copy_utf8_bounded(transcript, transcript_capacity, session->final_text);
        esp_err_t result = session->terminal_error;
        xSemaphoreGive(session->lock);
        return result;
    }
    return ESP_FAIL;
}

esp_err_t otool_speech_asr_cancel(otool_speech_asr_handle_t session)
{
    if (session == NULL) return ESP_ERR_INVALID_ARG;
    session->closing = true;
    set_terminal(session, ESP_ERR_INVALID_STATE);
    if (session->websocket != NULL && session->started) {
        esp_err_t err = esp_websocket_client_stop(session->websocket);
        session->started = false;
        session->connected = false;
        return err;
    }
    return ESP_OK;
}

void otool_speech_asr_close(otool_speech_asr_handle_t session)
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
    free(session->audio_packet);
    free(session->tx_buffer);
    free(session->ws_message);
    free(session->plain_response);
    free(session->final_text);
    otool_speech_gzip_encoder_destroy(session->gzip_encoder);
    otool_speech_gzip_decoder_destroy(session->gzip_decoder);
    if (session->connected_sem != NULL) vSemaphoreDelete(session->connected_sem);
    if (session->done_sem != NULL) vSemaphoreDelete(session->done_sem);
    if (session->lock != NULL) vSemaphoreDelete(session->lock);
    free(session);
}
