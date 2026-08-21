// otool_tab5_live2d LLM module: Doubao streaming chat worker + shared reply buffer.
// 不含任何 LVGL/UI 代码（界面由 ui_app 模块负责）。
// 线程模型：LLM worker task 阻塞执行 SDK 请求；回调只拷贝 delta 到共享 buffer；
// UI 通过 llm_app_reply_read()/llm_app_get_status() 读取。

#include "llm_app.h"
#include "wifi_app.h"
#include "credential_store.h"

#include "otool_llm_sdk.h"
#include "otool_llm_text.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <cstring>
#include <cstdio>

static const char *TAG = "llm_app";

/* 触摸/命令触发与打断 */
static SemaphoreHandle_t s_trigger_sem = nullptr;   /* 触发 → 立即提问/打断 */
static SemaphoreHandle_t s_request_lock = nullptr;  /* 保护 s_active_request / s_pending_question */
static otool_llm_request_handle_t s_active_request = nullptr;
static volatile int s_round = 0;

/* 自定义问题（console llm-ask <text> 注入，worker 取用一次后清除） */
static char s_pending_question[256] = { 0 };
static bool s_has_pending_question = false;

/* ---------------- 共享回复 buffer（worker 写，UI/console 读） ---------------- */

static constexpr size_t REPLY_BUF_CAP = 4096;

static char s_reply_buf[REPLY_BUF_CAP];
static size_t s_reply_len = 0;
static bool s_reply_truncated = false;
static char s_last_error[192] = { 0 };
static char s_hint[128] = { 0 };
static volatile bool s_request_busy = false;
static SemaphoreHandle_t s_reply_lock = nullptr;

static void reply_reset(void)
{
    if (xSemaphoreTake(s_reply_lock, portMAX_DELAY) == pdTRUE) {
        s_reply_len = 0;
        s_reply_buf[0] = '\0';
        s_reply_truncated = false;
        s_last_error[0] = '\0';
        s_hint[0] = '\0';
        xSemaphoreGive(s_reply_lock);
    }
}

static void reply_append(const char *data, size_t len)
{
    if (xSemaphoreTake(s_reply_lock, portMAX_DELAY) == pdTRUE) {
        size_t room = REPLY_BUF_CAP - 1 - s_reply_len;
        if (len >= room) {
            memcpy(s_reply_buf + s_reply_len, data, room);
            s_reply_len += room;
            s_reply_truncated = true;
        } else {
            memcpy(s_reply_buf + s_reply_len, data, len);
            s_reply_len += len;
        }
        s_reply_buf[s_reply_len] = '\0';
        xSemaphoreGive(s_reply_lock);
    }
}

static void reply_set_error(const char *message)
{
    if (xSemaphoreTake(s_reply_lock, portMAX_DELAY) == pdTRUE) {
        snprintf(s_last_error, sizeof(s_last_error), "%s", message);
        xSemaphoreGive(s_reply_lock);
    }
}

/* ---------------- 对外数据接口（线程安全） ---------------- */

extern "C" void llm_app_ask_now(void)
{
    if (s_trigger_sem != nullptr) {
        xSemaphoreGive(s_trigger_sem);
    }
}

extern "C" void llm_app_ask_text(const char *text)
{
    if (text == nullptr || text[0] == '\0') {
        llm_app_ask_now(); /* 空文本 = 默认问题 */
        return;
    }
    if (xSemaphoreTake(s_request_lock, portMAX_DELAY) == pdTRUE) {
        snprintf(s_pending_question, sizeof(s_pending_question), "%.255s", text);
        s_has_pending_question = true;
        xSemaphoreGive(s_request_lock);
    }
    if (s_trigger_sem != nullptr) {
        xSemaphoreGive(s_trigger_sem);
    }
}

extern "C" void llm_app_cancel_now(void)
{
    if (xSemaphoreTake(s_request_lock, portMAX_DELAY) == pdTRUE) {
        if (s_active_request != nullptr) {
            otool_llm_request_cancel(s_active_request);
        }
        xSemaphoreGive(s_request_lock);
    }
}

extern "C" void llm_app_get_status(llm_app_status_t *out)
{
    if (out == nullptr) {
        return;
    }
    if (xSemaphoreTake(s_reply_lock, portMAX_DELAY) == pdTRUE) {
        out->round = s_round;
        out->busy = s_request_busy;
        out->reply_len = s_reply_len;
        snprintf(out->error, sizeof(out->error), "%.127s", s_last_error);
        xSemaphoreGive(s_reply_lock);
    } else {
        memset(out, 0, sizeof(*out));
    }
}

extern "C" size_t llm_app_reply_read(char *buf, size_t cap)
{
    if (buf == nullptr || cap == 0) {
        return 0;
    }
    size_t copied = 0;
    if (xSemaphoreTake(s_reply_lock, portMAX_DELAY) == pdTRUE) {
        size_t n = s_reply_len < cap - 1 ? s_reply_len : cap - 1;
        memcpy(buf, s_reply_buf, n);
        buf[n] = '\0';
        copied = n;
        xSemaphoreGive(s_reply_lock);
    } else {
        buf[0] = '\0';
    }
    return copied;
}

extern "C" void llm_app_hint_read(char *buf, size_t cap)
{
    if (buf == nullptr || cap == 0) {
        return;
    }
    if (xSemaphoreTake(s_reply_lock, portMAX_DELAY) == pdTRUE) {
        snprintf(buf, cap, "%s", s_hint);
        xSemaphoreGive(s_reply_lock);
    } else {
        buf[0] = '\0';
    }
}

extern "C" void llm_app_set_hint(const char *text)
{
    if (xSemaphoreTake(s_reply_lock, portMAX_DELAY) == pdTRUE) {
        snprintf(s_hint, sizeof(s_hint), "%s", text ? text : "");
        xSemaphoreGive(s_reply_lock);
    }
}

/* ---------------- LLM worker ---------------- */

static otool_llm_event_action_t llm_on_event(const otool_llm_text_event_t *evt, void *user_ctx)
{
    (void)user_ctx;
    switch (evt->type) {
    case OTOOL_LLM_TEXT_EVENT_RESPONSE_STARTED:
        ESP_LOGI(TAG, "response started: %s", evt->response_id ? evt->response_id : "?");
        break;
    case OTOOL_LLM_TEXT_EVENT_TEXT_DELTA:
        reply_append(evt->data.text_delta.data, evt->data.text_delta.data_len);
        /* 关键信息：流式回复实时打印到 console（stdout = USB-Serial-JTAG） */
        printf("%.*s", (int)evt->data.text_delta.data_len, evt->data.text_delta.data);
        fflush(stdout);
        break;
    case OTOOL_LLM_TEXT_EVENT_TEXT_DONE:
        printf("\n");
        fflush(stdout);
        break;
    case OTOOL_LLM_TEXT_EVENT_USAGE:
        ESP_LOGI(TAG, "usage: in=%lld out=%lld total=%lld",
                 (long long)evt->data.usage.input_tokens,
                 (long long)evt->data.usage.output_tokens,
                 (long long)evt->data.usage.total_tokens);
        break;
    case OTOOL_LLM_TEXT_EVENT_COMPLETED:
        ESP_LOGI(TAG, "completed");
        break;
    case OTOOL_LLM_TEXT_EVENT_INCOMPLETE:
        ESP_LOGI(TAG, "incomplete: %s", evt->data.incomplete.reason ? evt->data.incomplete.reason : "?");
        break;
    case OTOOL_LLM_TEXT_EVENT_CANCELLED:
        ESP_LOGI(TAG, "cancelled");
        break;
    case OTOOL_LLM_TEXT_EVENT_ERROR:
        ESP_LOGE(TAG, "error: %s (code=%s)", evt->data.error.message ? evt->data.error.message : "?",
                 otool_llm_err_to_name(evt->data.error.code));
        printf("[llm] error: %s\n", evt->data.error.message ? evt->data.error.message : "?");
        fflush(stdout);
        reply_set_error(evt->data.error.message ? evt->data.error.message : "llm error");
        break;
    default:
        break;
    }
    return OTOOL_LLM_EVENT_ACTION_CONTINUE;
}

static void llm_worker_task(void *arg)
{
    (void)arg;

    /* 等待 Wi-Fi 就绪（最多 30s，由 wifi_app 管理连接状态） */
    if (wifi_app_wait_connected(30000) != ESP_OK) {
        ESP_LOGE(TAG, "wifi not connected within 30s, LLM disabled");
        vTaskDelete(nullptr);
        return;
    }

    /* 运行时凭证（NVS，console 'cred set llm_key <key>'） */
    const char *api_key = credential_llm_key();
    if (api_key == nullptr || api_key[0] == '\0') {
        ESP_LOGE(TAG, "llm api key not set; run 'cred set llm_key <key>' then reboot");
        vTaskDelete(nullptr);
        return;
    }

    otool_llm_client_config_t cfg = {};
    cfg.struct_size = sizeof(cfg);
    cfg.provider = OTOOL_LLM_PROVIDER_VOLCENGINE_ARK;
    cfg.protocol = OTOOL_LLM_PROTOCOL_AUTO;
    cfg.api_key = api_key;
    cfg.connect_timeout_ms = 15000;
    cfg.read_timeout_ms = 60000;

    otool_llm_client_handle_t client = nullptr;
    esp_err_t err = otool_llm_client_create(&cfg, &client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "client create failed: %s", esp_err_to_name(err));
        vTaskDelete(nullptr);
        return;
    }

    const char *questions[] = {
        "你好，请用一句话介绍你自己。",
        "1+1等于几？请只回答数字。",
        "请用三个要点说明什么是流式输出。",
    };

    for (int round = 0;; round++) {
        /* 事件驱动：等待触发（点击屏幕 / console llm-ask），不再自动轮询调试 */
        xSemaphoreTake(s_trigger_sem, portMAX_DELAY);

        /* 触发时若上一轮仍在请求，先跨任务打断（SDK 取消能力） */
        if (xSemaphoreTake(s_request_lock, portMAX_DELAY) == pdTRUE) {
            if (s_active_request != nullptr) {
                ESP_LOGI(TAG, "trigger: cancelling in-flight request");
                otool_llm_request_cancel(s_active_request);
            }
            xSemaphoreGive(s_request_lock);
        }
        int waited = 0;
        while (s_request_busy && waited++ < 300) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        s_round = round;

        /* 取问题：优先自定义（console llm-ask <text>），否则默认轮换 */
        char custom_question[256] = { 0 };
        const char *question = nullptr;
        if (xSemaphoreTake(s_request_lock, portMAX_DELAY) == pdTRUE) {
            if (s_has_pending_question) {
                memcpy(custom_question, s_pending_question, sizeof(custom_question));
                s_has_pending_question = false;
                s_pending_question[0] = '\0';
            }
            xSemaphoreGive(s_request_lock);
        }
        if (custom_question[0] != '\0') {
            question = custom_question;
        } else {
            question = questions[round % (sizeof(questions) / sizeof(questions[0]))];
        }

        reply_reset();
        s_request_busy = true;

        otool_llm_text_message_t msg = { .role = OTOOL_LLM_ROLE_USER, .text = question };
        otool_llm_text_request_t req = {};
        req.struct_size = sizeof(req);
        req.model = CONFIG_OTOOL_LLM_MODEL;
        req.messages = &msg;
        req.message_count = 1;
        req.max_output_tokens = 2048;

        otool_llm_request_handle_t request = nullptr;
        err = otool_llm_request_create(client, &req, &request);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "request create failed: %s", esp_err_to_name(err));
            reply_set_error("request create failed");
            s_request_busy = false;
            continue; /* 回到等待触发状态 */
        }

        /* 注册为可打断的当前请求 */
        if (xSemaphoreTake(s_request_lock, portMAX_DELAY) == pdTRUE) {
            s_active_request = request;
            xSemaphoreGive(s_request_lock);
        }

        ESP_LOGI(TAG, "round %d: ask '%s'", round, question);
        printf("[llm] round %d ask: %s\n", round, question);
        fflush(stdout);
        err = otool_llm_request_execute_stream(request, llm_on_event, nullptr);
        ESP_LOGI(TAG, "round %d done: %s, reply_len=%u", round, esp_err_to_name(err),
                 (unsigned)s_reply_len);
        printf("[llm] round %d done: %s, reply_len=%u bytes\n", round, esp_err_to_name(err),
               (unsigned)s_reply_len);
        fflush(stdout);

        if (xSemaphoreTake(s_request_lock, portMAX_DELAY) == pdTRUE) {
            s_active_request = nullptr;
            xSemaphoreGive(s_request_lock);
        }

        otool_llm_request_destroy(request);
        s_request_busy = false;
    }
}

/* ---------------- entry ---------------- */

extern "C" void llm_app_start(void)
{
    s_reply_lock = xSemaphoreCreateMutex();
    s_request_lock = xSemaphoreCreateMutex();
    s_trigger_sem = xSemaphoreCreateCounting(4, 0);

    BaseType_t created = xTaskCreatePinnedToCore(llm_worker_task, "llm_worker",
                                                 CONFIG_OTOOL_LLM_LLM_TASK_STACK_SIZE, nullptr, 5,
                                                 nullptr, 1);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "llm worker task create failed");
    }
}
