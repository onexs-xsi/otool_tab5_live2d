// otool_tab5_live2d LLM demo app: ESP-Hosted (C6 Wi-Fi) + Doubao streaming chat + LVGL UI.
// 线程模型：LLM worker task 阻塞执行 SDK 请求；回调只拷贝 delta 到共享 buffer；
// LVGL timer（LVGL 上下文）把 buffer 同步到界面，不阻塞 UI。

#include "llm_app.h"

#include "otool_esp_hosted_fw_update.h"
#include "otool_llm_sdk.h"
#include "otool_llm_text.h"
#include "otool_lvgl_port.h"
#include "otool_tab5_component.h"
#include "lvgl.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <cstring>
#include <cstdio>

static const char *TAG = "llm_app";

static constexpr EventBits_t WIFI_CONNECTED_BIT = BIT0;

static EventGroupHandle_t s_wifi_events = nullptr;
static int s_wifi_retries = 0;
static void *s_tab5_comp = nullptr;

/* 触摸触发与打断 */
static SemaphoreHandle_t s_trigger_sem = nullptr;   /* 点击屏幕 → 立即提问/打断 */
static SemaphoreHandle_t s_request_lock = nullptr;  /* 保护 s_active_request */
static otool_llm_request_handle_t s_active_request = nullptr;
static volatile int s_round = 0;

/* ---------------- 共享回复 buffer（worker 写，LVGL timer 读） ---------------- */

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

static void reply_clear_error(void)
{
    if (xSemaphoreTake(s_reply_lock, portMAX_DELAY) == pdTRUE) {
        s_last_error[0] = '\0';
        xSemaphoreGive(s_reply_lock);
    }
}

/* ---------------- console / UI 控制入口 ---------------- */

extern "C" void llm_app_ask_now(void)
{
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

extern "C" void llm_app_status_str(char *buf, size_t size)
{
    if (xSemaphoreTake(s_reply_lock, portMAX_DELAY) == pdTRUE) {
        snprintf(buf, size, "round=%d busy=%d reply_len=%u err=%s", s_round,
                 (int)s_request_busy, (unsigned)s_reply_len,
                 s_last_error[0] ? s_last_error : "-");
        xSemaphoreGive(s_reply_lock);
    } else {
        snprintf(buf, size, "status unavailable");
    }
}

extern "C" void llm_app_set_hint(const char *text)
{
    if (xSemaphoreTake(s_reply_lock, portMAX_DELAY) == pdTRUE) {
        snprintf(s_hint, sizeof(s_hint), "%s", text ? text : "");
        xSemaphoreGive(s_reply_lock);
    }
}

/* ---------------- Wi-Fi（ESP-Hosted / C6） ---------------- */

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA start, connecting to %s", CONFIG_OTOOL_WIFI_SSID);
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_retries < CONFIG_OTOOL_WIFI_MAX_RETRY) {
            s_wifi_retries++;
            ESP_LOGW(TAG, "disconnected (retry %d/%d), reconnecting...", s_wifi_retries,
                     CONFIG_OTOOL_WIFI_MAX_RETRY);
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "wifi give up after %d retries", s_wifi_retries);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_retries = 0;
        /* 降低发射功率（10 dBm），减少 USB 供电下的电流尖峰（HP WDT 复位缓解） */
        esp_err_t perr = esp_wifi_set_max_tx_power(40);
        if (perr != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_set_max_tx_power: %s", esp_err_to_name(perr));
        }
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_sta_start(void)
{
    /* C6 模块上电：WLAN_PWR_EN 由 IO 扩展器（ADDR_HIGH 0x44 P0）控制。
     * 参考 c145_tab5_wifi_module_update_ui_project：先上电并等待 1s 稳定，
     * 再初始化 ESP-Hosted（SDIO 链路）。 */
    m5::tab5::otool_tab5_component *comp =
        (m5::tab5::otool_tab5_component *)s_tab5_comp;
    if (comp != nullptr) {
        esp_err_t perr = comp->wlan_power(true);
        if (perr != ESP_OK) {
            ESP_LOGW(TAG, "wlan_power(true) failed: %s", esp_err_to_name(perr));
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "ESP-Hosted minimal init (C6 SDIO)...");
    esp_err_t err = otool_esp_hosted_fw_update_minimal_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hosted minimal init failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr);

    wifi_config_t wifi_config = {};
    snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", CONFIG_OTOOL_WIFI_SSID);
    snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s",
             CONFIG_OTOOL_WIFI_PASSWORD);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        return err;
    }
    return esp_wifi_start();
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
        break;
    case OTOOL_LLM_TEXT_EVENT_TEXT_DONE:
        ESP_LOGI(TAG, "text done");
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

    /* 等待 Wi-Fi 就绪（最多 30s） */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(30000));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "wifi not connected within 30s, LLM disabled");
        vTaskDelete(nullptr);
        return;
    }

    otool_llm_client_config_t cfg = {};
    cfg.struct_size = sizeof(cfg);
    cfg.provider = OTOOL_LLM_PROVIDER_VOLCENGINE_ARK;
    cfg.protocol = OTOOL_LLM_PROTOCOL_AUTO;
    cfg.api_key = CONFIG_OTOOL_LLM_API_KEY;
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
        /* 等待触发（点击屏幕）或 30s 自动进入下一轮 */
        if (xSemaphoreTake(s_trigger_sem, pdMS_TO_TICKS(30000)) == pdTRUE) {
            /* 点击触发：若正在请求，先跨任务打断（SDK 取消能力） */
            if (xSemaphoreTake(s_request_lock, portMAX_DELAY) == pdTRUE) {
                if (s_active_request != nullptr) {
                    ESP_LOGI(TAG, "tap: cancelling in-flight request");
                    otool_llm_request_cancel(s_active_request);
                }
                xSemaphoreGive(s_request_lock);
            }
            int waited = 0;
            while (s_request_busy && waited++ < 300) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }

        s_round = round;
        const char *question = questions[round % (sizeof(questions) / sizeof(questions[0]))];

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
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        /* 注册为可打断的当前请求 */
        if (xSemaphoreTake(s_request_lock, portMAX_DELAY) == pdTRUE) {
            s_active_request = request;
            xSemaphoreGive(s_request_lock);
        }

        ESP_LOGI(TAG, "round %d: ask '%s'", round, question);
        err = otool_llm_request_execute_stream(request, llm_on_event, nullptr);
        ESP_LOGI(TAG, "round %d done: %s, reply_len=%u", round, esp_err_to_name(err),
                 (unsigned)s_reply_len);

        if (xSemaphoreTake(s_request_lock, portMAX_DELAY) == pdTRUE) {
            s_active_request = nullptr;
            xSemaphoreGive(s_request_lock);
        }

        otool_llm_request_destroy(request);
        s_request_busy = false;
    }
}

/* ---------------- UI ---------------- */

static lv_obj_t *s_status_label = nullptr;
static lv_obj_t *s_reply_label = nullptr;
static lv_obj_t *s_hint_label = nullptr;
static char s_status_text[192] = "starting...";

/* 点击屏幕：触发提问 / 打断当前请求 */
static void screen_click_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "tap detected");
    xSemaphoreGive(s_trigger_sem);
}

static void ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_reply_label == nullptr) {
        return;
    }

    if (xSemaphoreTake(s_reply_lock, portMAX_DELAY) == pdTRUE) {
        if (s_last_error[0] != '\0') {
            snprintf(s_status_text, sizeof(s_status_text), "round %d | error: %.120s", s_round,
                     s_last_error);
        } else if (s_request_busy) {
            snprintf(s_status_text, sizeof(s_status_text), "round %d | LLM streaming...", s_round);
        } else {
            snprintf(s_status_text, sizeof(s_status_text), "round %d | LLM idle | tap to ask",
                     s_round);
        }
        if (s_hint[0] != '\0') {
            lv_label_set_text(s_hint_label, s_hint);
        }
        lv_label_set_text(s_status_label, s_status_text);
        lv_label_set_text(s_reply_label, s_reply_buf);
        xSemaphoreGive(s_reply_lock);
    }
}

static void ui_build(void)
{
    otool_lvgl_port_lock(0);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_pad_all(scr, 16, 0);

    /* 状态行 */
    s_status_label = lv_label_create(scr);
    lv_label_set_text(s_status_label, "starting...");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x8fa3c0), 0);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_LEFT, 0, 0);

    /* 触摸提示行 */
    s_hint_label = lv_label_create(scr);
    lv_label_set_text(s_hint_label, "tap screen to ask / interrupt");
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(0x55607a), 0);
    lv_obj_align(s_hint_label, LV_ALIGN_TOP_RIGHT, 0, 0);

    /* 回复区 */
    s_reply_label = lv_label_create(scr);
    lv_label_set_text(s_reply_label, "");
    lv_label_set_long_mode(s_reply_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_reply_label, lv_pct(100));
    lv_obj_set_style_text_color(s_reply_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_reply_label, &lv_font_source_han_sans_sc_16_cjk, 0);
    lv_obj_align(s_reply_label, LV_ALIGN_TOP_LEFT, 0, 40);
    lv_obj_set_height(s_reply_label, lv_pct(80));
    lv_obj_set_style_text_line_space(s_reply_label, 6, 0);

    /* 全屏触摸：点击提问 / 打断 */
    lv_obj_add_event_cb(scr, screen_click_cb, LV_EVENT_CLICKED, nullptr);

    otool_lvgl_port_unlock();

    lv_timer_create(ui_timer_cb, CONFIG_OTOOL_LLM_UI_REFRESH_MS, nullptr);
}

/* ---------------- entry ---------------- */

extern "C" void llm_app_start(void *tab5_comp)
{
    s_tab5_comp = tab5_comp;
    s_wifi_events = xEventGroupCreate();
    s_reply_lock = xSemaphoreCreateMutex();
    s_request_lock = xSemaphoreCreateMutex();
    s_trigger_sem = xSemaphoreCreateCounting(4, 0);

    ui_build();

    esp_err_t err = wifi_sta_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi init failed: %s", esp_err_to_name(err));
        reply_set_error("wifi init failed");
        return;
    }

    BaseType_t created = xTaskCreatePinnedToCore(llm_worker_task, "llm_worker",
                                                 CONFIG_OTOOL_LLM_LLM_TASK_STACK_SIZE, nullptr, 5,
                                                 nullptr, 1);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "llm worker task create failed");
    }
}
