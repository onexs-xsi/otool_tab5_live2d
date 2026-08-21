/*
 * SPDX-FileCopyrightText: 2026 otool project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Transport test battery for otool_llm_sdk. Runs against local_sse_server.py.
 *
 * Works on the IDF linux target (host sockets) and on esp32 targets that have
 * a working network path (Wi-Fi/Ethernet must be brought up by the board).
 */

#include "otool_llm_sdk.h"
#include "otool_llm_text.h"

#include "esp_log.h"
#include "sdkconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CONFIG_IDF_TARGET_LINUX
#include "esp_event.h"
#include "esp_netif.h"
#endif

static const char *TAG = "llm_test";

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        g_checks++;                                                             \
        if (!(cond)) {                                                          \
            g_failures++;                                                       \
            ESP_LOGE(TAG, "FAIL: " __VA_ARGS__);                                \
        }                                                                       \
    } while (0)

#define CHECK_STR(a, b) CHECK(strcmp((a), (b)) == 0, "expected \"%s\", got \"%s\"", (b), (a))

typedef struct {
    char delta_buf[2048];
    size_t delta_len;
    otool_llm_text_event_type_t terminal;
    esp_err_t terminal_code;
    char error_msg[256];
    char provider_code[64];
    char request_id[128];
    bool got_started;
    bool got_done;
    bool got_usage;
    int64_t usage_total;
    int delta_count;
} test_state_t;

static otool_llm_event_action_t on_event(const otool_llm_text_event_t *evt, void *arg)
{
    test_state_t *s = (test_state_t *)arg;
    switch (evt->type) {
    case OTOOL_LLM_TEXT_EVENT_RESPONSE_STARTED:
        s->got_started = true;
        ESP_LOGI(TAG, "  started: response_id=%s model=%s",
                 evt->response_id ? evt->response_id : "?", evt->model ? evt->model : "?");
        break;
    case OTOOL_LLM_TEXT_EVENT_TEXT_DELTA:
        if (s->delta_len + evt->data.text_delta.data_len < sizeof(s->delta_buf)) {
            memcpy(s->delta_buf + s->delta_len, evt->data.text_delta.data, evt->data.text_delta.data_len);
            s->delta_len += evt->data.text_delta.data_len;
            s->delta_buf[s->delta_len] = '\0';
        }
        s->delta_count++;
        break;
    case OTOOL_LLM_TEXT_EVENT_TEXT_DONE:
        s->got_done = true;
        break;
    case OTOOL_LLM_TEXT_EVENT_USAGE:
        s->got_usage = true;
        s->usage_total = evt->data.usage.total_tokens;
        break;
    case OTOOL_LLM_TEXT_EVENT_COMPLETED:
        s->terminal = evt->type;
        ESP_LOGI(TAG, "  completed");
        break;
    case OTOOL_LLM_TEXT_EVENT_INCOMPLETE:
        s->terminal = evt->type;
        ESP_LOGI(TAG, "  incomplete: reason=%s", evt->data.incomplete.reason ? evt->data.incomplete.reason : "?");
        break;
    case OTOOL_LLM_TEXT_EVENT_CANCELLED:
        s->terminal = evt->type;
        ESP_LOGI(TAG, "  cancelled");
        break;
    case OTOOL_LLM_TEXT_EVENT_ERROR:
        s->terminal = evt->type;
        s->terminal_code = evt->data.error.code;
        snprintf(s->error_msg, sizeof(s->error_msg), "%s", evt->data.error.message ? evt->data.error.message : "");
        if (evt->data.error.provider_code) {
            snprintf(s->provider_code, sizeof(s->provider_code), "%s", evt->data.error.provider_code);
        }
        if (evt->request_id) {
            snprintf(s->request_id, sizeof(s->request_id), "%s", evt->request_id);
        }
        ESP_LOGI(TAG, "  error: code=%d (%s) msg=%s provider_code=%s request_id=%s",
                 (int)evt->data.error.code, otool_llm_err_to_name(evt->data.error.code),
                 s->error_msg, s->provider_code, s->request_id);
        break;
    default:
        break;
    }
    return OTOOL_LLM_EVENT_ACTION_CONTINUE;
}

static void state_init(test_state_t *s)
{
    memset(s, 0, sizeof(*s));
    s->terminal_code = ESP_OK;
}

static esp_err_t run_case(const char *name, const char *path, otool_llm_protocol_t protocol,
                          otool_llm_text_event_type_t expect_terminal, esp_err_t expect_code,
                          int expect_delta_count, const char *expect_delta, int expect_usage)
{
    test_state_t st;
    state_init(&st);

    char url[256];
    snprintf(url, sizeof(url), "http://%s:%d", CONFIG_OTOOL_LLM_TEST_SERVER_HOST,
             CONFIG_OTOOL_LLM_TEST_SERVER_PORT);

    otool_llm_client_config_t cfg = {
        .struct_size = sizeof(cfg),
        .provider = OTOOL_LLM_PROVIDER_CUSTOM,
        .protocol = protocol,
        .base_url = url,
        .chat_path = path,
        .responses_path = path,
        .api_key = "test-key-123",
        .connect_timeout_ms = 3000,
        .read_timeout_ms = 8000,
    };
    otool_llm_client_handle_t client = NULL;
    esp_err_t err = otool_llm_client_create(&cfg, &client);
    CHECK(err == ESP_OK, "%s: client create", name);
    if (err != ESP_OK) {
        return err;
    }

    otool_llm_text_message_t msg = { .role = OTOOL_LLM_ROLE_USER, .text = "hi" };
    otool_llm_text_request_t req = {
        .struct_size = sizeof(req),
        .model = "local-model",
        .messages = &msg,
        .message_count = 1,
        .max_output_tokens = 64,
    };
    otool_llm_request_handle_t request = NULL;
    err = otool_llm_request_create(client, &req, &request);
    CHECK(err == ESP_OK, "%s: request create", name);

    err = otool_llm_request_execute_stream(request, on_event, &st);

    CHECK(st.terminal == expect_terminal, "%s: terminal expected type %d got %d", name,
          (int)expect_terminal, (int)st.terminal);
    if (expect_terminal == OTOOL_LLM_TEXT_EVENT_ERROR) {
        CHECK(st.terminal_code == expect_code, "%s: error code expected 0x%x got 0x%x", name,
              (unsigned)expect_code, (unsigned)st.terminal_code);
        CHECK(err == expect_code, "%s: execute return expected 0x%x got 0x%x", name,
              (unsigned)expect_code, (unsigned)err);
    } else {
        CHECK(err == ESP_OK, "%s: execute return expected ESP_OK got 0x%x", name, (unsigned)err);
    }
    if (expect_delta_count >= 0) {
        CHECK(st.delta_count == expect_delta_count, "%s: delta count expected %d got %d", name,
              expect_delta_count, st.delta_count);
    }
    if (expect_delta != NULL) {
        CHECK_STR(st.delta_buf, expect_delta);
    }
    if (expect_usage > 0) {
        CHECK(st.got_usage && st.usage_total == expect_usage, "%s: usage expected %d got %d", name,
              expect_usage, (int)st.usage_total);
    }

    otool_llm_request_destroy(request);
    otool_llm_client_destroy(client);
    return err;
}

/* ---- cancel tests ---- */

typedef struct {
    otool_llm_request_handle_t request;
    test_state_t *state;
    volatile bool start_cancel;
} cancel_args_t;

static void cancel_task(void *arg)
{
    cancel_args_t *ca = (cancel_args_t *)arg;
    while (!ca->start_cancel) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    /* cancel after the first delta arrived */
    esp_err_t err = otool_llm_request_cancel(ca->request);
    ESP_LOGI(TAG, "cancel task: otool_llm_request_cancel -> %s", esp_err_to_name(err));
    vTaskDelete(NULL);
}

static void test_cancel(int iteration)
{
    test_state_t st;
    state_init(&st);

    char url[256];
    snprintf(url, sizeof(url), "http://%s:%d", CONFIG_OTOOL_LLM_TEST_SERVER_HOST,
             CONFIG_OTOOL_LLM_TEST_SERVER_PORT);

    otool_llm_client_config_t cfg = {
        .struct_size = sizeof(cfg),
        .provider = OTOOL_LLM_PROVIDER_CUSTOM,
        .protocol = OTOOL_LLM_PROTOCOL_RESPONSES_SSE,
        .base_url = url,
        .responses_path = "/slow",
        .chat_path = "/slow",
        .api_key = "test-key-123",
        .connect_timeout_ms = 3000,
        .read_timeout_ms = 15000,
    };
    otool_llm_client_handle_t client = NULL;
    CHECK(otool_llm_client_create(&cfg, &client) == ESP_OK, "cancel%d: client create", iteration);

    otool_llm_text_message_t msg = { .role = OTOOL_LLM_ROLE_USER, .text = "hi" };
    otool_llm_text_request_t req = {
        .struct_size = sizeof(req),
        .model = "local-model",
        .messages = &msg,
        .message_count = 1,
    };
    otool_llm_request_handle_t request = NULL;
    CHECK(otool_llm_request_create(client, &req, &request) == ESP_OK, "cancel%d: request create", iteration);

    cancel_args_t ca = {
        .request = request,
        .state = &st,
        .start_cancel = false,
    };
    CHECK(xTaskCreate(cancel_task, "cancel", 4096, &ca, 5, NULL) == pdPASS, "cancel%d: task create", iteration);

    esp_err_t err = otool_llm_request_execute_stream(request, on_event, &st);
    ca.start_cancel = true;

    CHECK(st.terminal == OTOOL_LLM_TEXT_EVENT_CANCELLED, "cancel%d: terminal expected CANCELLED got %d",
          iteration, (int)st.terminal);
    CHECK(err == ESP_OK, "cancel%d: execute return expected ESP_OK got 0x%x", iteration, (unsigned)err);
    CHECK(st.delta_count >= 1, "cancel%d: expected at least one delta before cancel", iteration);
    CHECK(st.got_started, "cancel%d: response started before cancel", iteration);

    /* idempotent re-cancel after completion */
    CHECK(otool_llm_request_cancel(request) == ESP_OK, "cancel%d: re-cancel idempotent", iteration);

    otool_llm_request_destroy(request);
    otool_llm_client_destroy(client);
    vTaskDelay(pdMS_TO_TICKS(50));
}

/* ---- concurrency test: one in-flight request per client ---- */

typedef struct {
    otool_llm_request_handle_t request;
    test_state_t *state;
    esp_err_t result;
    volatile bool done;
} worker_args_t;

static void worker_task(void *arg)
{
    worker_args_t *w = (worker_args_t *)arg;
    w->result = otool_llm_request_execute_stream(w->request, on_event, w->state);
    w->done = true;
    vTaskDelete(NULL);
}

static void test_client_busy(void)
{
    test_state_t st1;
    state_init(&st1);

    char url[256];
    snprintf(url, sizeof(url), "http://%s:%d", CONFIG_OTOOL_LLM_TEST_SERVER_HOST,
             CONFIG_OTOOL_LLM_TEST_SERVER_PORT);

    otool_llm_client_config_t cfg = {
        .struct_size = sizeof(cfg),
        .provider = OTOOL_LLM_PROVIDER_CUSTOM,
        .protocol = OTOOL_LLM_PROTOCOL_RESPONSES_SSE,
        .base_url = url,
        .responses_path = "/slow",
        .chat_path = "/slow",
        .api_key = "test-key-123",
        .connect_timeout_ms = 3000,
        .read_timeout_ms = 15000,
    };
    otool_llm_client_handle_t client = NULL;
    CHECK(otool_llm_client_create(&cfg, &client) == ESP_OK, "busy: client create");

    otool_llm_text_message_t msg = { .role = OTOOL_LLM_ROLE_USER, .text = "hi" };
    otool_llm_text_request_t req = {
        .struct_size = sizeof(req),
        .model = "local-model",
        .messages = &msg,
        .message_count = 1,
    };
    otool_llm_request_handle_t r1 = NULL, r2 = NULL;
    CHECK(otool_llm_request_create(client, &req, &r1) == ESP_OK, "busy: r1 create");
    CHECK(otool_llm_request_create(client, &req, &r2) == ESP_OK, "busy: r2 create");

    worker_args_t w = {
        .request = r1,
        .state = &st1,
        .result = ESP_FAIL,
        .done = false,
    };
    CHECK(xTaskCreate(worker_task, "worker", 8192, &w, 5, NULL) == pdPASS, "busy: worker create");

    /* wait until the first request is in flight (first delta) */
    int waited = 0;
    while (!st1.got_started && waited < 200) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited++;
    }
    CHECK(st1.got_started, "busy: r1 started");

    /* second execute on the same client must be refused */
    esp_err_t err = otool_llm_request_execute_stream(r2, on_event, NULL);
    CHECK(err == ESP_ERR_INVALID_STATE, "busy: concurrent execute refused (got 0x%x)", (unsigned)err);

    /* the first request completes normally */
    while (!w.done) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    CHECK(w.result == ESP_OK && st1.terminal == OTOOL_LLM_TEXT_EVENT_COMPLETED, "busy: r1 completed");

    otool_llm_request_destroy(r1);
    otool_llm_request_destroy(r2);
    otool_llm_client_destroy(client);
}

/* ---- request build / HTTPS enforcement (no network) ---- */

static void test_config_validation(void)
{
    /* custom + AUTO must be rejected */
    otool_llm_client_config_t cfg = {
        .struct_size = sizeof(cfg),
        .provider = OTOOL_LLM_PROVIDER_CUSTOM,
        .protocol = OTOOL_LLM_PROTOCOL_AUTO,
        .base_url = "http://localhost:1",
        .api_key = "k",
    };
    otool_llm_client_handle_t client = NULL;
    CHECK(otool_llm_client_create(&cfg, &client) == ESP_ERR_INVALID_ARG, "custom+auto rejected");

    /* missing api key rejected */
    cfg.protocol = OTOOL_LLM_PROTOCOL_RESPONSES_SSE;
    cfg.api_key = NULL;
    CHECK(otool_llm_client_create(&cfg, &client) == ESP_ERR_INVALID_ARG, "missing key rejected");

    /* small struct_size rejected */
    cfg.api_key = "k";
    cfg.struct_size = 4;
    CHECK(otool_llm_client_create(&cfg, &client) == ESP_ERR_INVALID_VERSION, "small struct_size rejected");

    /* chat protocol rejects responses-only fields */
    cfg.struct_size = sizeof(cfg);
    cfg.protocol = OTOOL_LLM_PROTOCOL_CHAT_COMPLETIONS_SSE;
    otool_llm_client_handle_t c2 = NULL;
    CHECK(otool_llm_client_create(&cfg, &c2) == ESP_OK, "valid custom client");
    otool_llm_text_message_t msg = { .role = OTOOL_LLM_ROLE_USER, .text = "hi" };
    otool_llm_text_request_t req = {
        .struct_size = sizeof(req),
        .model = "m",
        .messages = &msg,
        .message_count = 1,
        .previous_response_id = "x",
    };
    otool_llm_request_handle_t r = NULL;
    CHECK(otool_llm_request_create(c2, &req, &r) == OTOOL_LLM_ERR_UNSUPPORTED,
          "chat + previous_response_id rejected");
    otool_llm_client_destroy(c2);
}

#ifdef CONFIG_IDF_TARGET_LINUX
static void network_init(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
}
#else
static void network_init(void)
{
    /* The application is responsible for bringing up Wi-Fi/Ethernet first. */
    ESP_LOGW(TAG, "no network init on this target: connect the board to a network before running");
}
#endif

void app_main(void)
{
    ESP_LOGI(TAG, "otool_llm_sdk transport test battery");
    network_init();
    vTaskDelay(pdMS_TO_TICKS(200));

    /* happy paths */
    run_case("ok-responses", "/ok", OTOOL_LLM_PROTOCOL_RESPONSES_SSE,
             OTOOL_LLM_TEXT_EVENT_COMPLETED, ESP_OK, 3, "你好，world 🌍 (3rd chunk)", 30);
    run_case("ok-chat", "/chat", OTOOL_LLM_PROTOCOL_CHAT_COMPLETIONS_SSE,
             OTOOL_LLM_TEXT_EVENT_COMPLETED, ESP_OK, 2, "Hi from chat", 10);

    /* error shapes */
    run_case("err-401", "/401", OTOOL_LLM_PROTOCOL_RESPONSES_SSE,
             OTOOL_LLM_TEXT_EVENT_ERROR, OTOOL_LLM_ERR_PROVIDER, -1, NULL, 0);
    run_case("err-429", "/429", OTOOL_LLM_PROTOCOL_RESPONSES_SSE,
             OTOOL_LLM_TEXT_EVENT_ERROR, OTOOL_LLM_ERR_PROVIDER, -1, NULL, 0);
    run_case("err-500", "/500", OTOOL_LLM_PROTOCOL_RESPONSES_SSE,
             OTOOL_LLM_TEXT_EVENT_ERROR, OTOOL_LLM_ERR_PROVIDER, -1, NULL, 0);
    run_case("err-badtype", "/badtype", OTOOL_LLM_PROTOCOL_RESPONSES_SSE,
             OTOOL_LLM_TEXT_EVENT_ERROR, OTOOL_LLM_ERR_BAD_CONTENT_TYPE, -1, NULL, 0);
    run_case("err-half", "/half", OTOOL_LLM_PROTOCOL_RESPONSES_SSE,
             OTOOL_LLM_TEXT_EVENT_ERROR, OTOOL_LLM_ERR_PROTOCOL_EOF, -1, NULL, 0);
    run_case("err-oversize", "/oversize", OTOOL_LLM_PROTOCOL_RESPONSES_SSE,
             OTOOL_LLM_TEXT_EVENT_ERROR, OTOOL_LLM_ERR_EVENT_TOO_LARGE, -1, NULL, 0);
    run_case("err-errorjson", "/errorjson", OTOOL_LLM_PROTOCOL_RESPONSES_SSE,
             OTOOL_LLM_TEXT_EVENT_ERROR, OTOOL_LLM_ERR_JSON, -1, NULL, 0);
    run_case("err-eof-no-terminal", "/eof-no-terminal", OTOOL_LLM_PROTOCOL_RESPONSES_SSE,
             OTOOL_LLM_TEXT_EVENT_ERROR, OTOOL_LLM_ERR_PROTOCOL_EOF, -1, NULL, 0);
    run_case("err-multi-choice", "/multi-choice", OTOOL_LLM_PROTOCOL_CHAT_COMPLETIONS_SSE,
             OTOOL_LLM_TEXT_EVENT_ERROR, OTOOL_LLM_ERR_UNSUPPORTED, -1, NULL, 0);

    test_cancel(1);
    test_cancel(2);
    test_cancel(3);
    test_client_busy();
    test_config_validation();

    ESP_LOGI(TAG, "==============================================");
    ESP_LOGI(TAG, "RESULT: %d checks, %d failures", g_checks, g_failures);
    ESP_LOGI(TAG, "==============================================");

#ifdef CONFIG_IDF_TARGET_LINUX
    /* the harness exits with the result */
    exit(g_failures == 0 ? 0 : 1);
#endif
}
