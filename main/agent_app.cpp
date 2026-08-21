// Agent app: 设备工具 + Agent worker（WP6）。
// Agent 事件全部打印到 console（LLM 测试以 console 为主）；最终回复累积到
// 共享 buffer 供 UI 显示；工具 get_device_status 为只读（uptime/堆/网络）。

#include "agent_app.h"
#include "credential_store.h"
#include "wifi_app.h"

#include "otool_llm_agent.h"
#include "otool_llm_sdk.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <cstdio>
#include <cstring>

static const char *TAG = "agent_app";

static constexpr size_t AGENT_REPLY_CAP = 4096;

static SemaphoreHandle_t s_trigger_sem = nullptr;   /* agent <text> 触发 */
static SemaphoreHandle_t s_reply_lock = nullptr;    /* 回复/状态缓冲锁 */
static char s_pending_question[256] = { 0 };
static char s_reply_buf[AGENT_REPLY_CAP];
static size_t s_reply_len = 0;
static char s_last_error[192] = { 0 };
static volatile bool s_busy = false;
static volatile int s_round = 0;

/* ---------------- 设备工具（只读） ---------------- */

static esp_err_t tool_get_device_status(const char *arguments_json, char *output_json,
                                        size_t output_capacity, size_t *output_length,
                                        const otool_llm_tool_exec_context_t *exec_ctx,
                                        void *user_ctx)
{
    (void)arguments_json;
    (void)exec_ctx;
    (void)user_ctx;
    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    int heap_free = (int)esp_get_free_heap_size();
    snprintf(output_json, output_capacity,
             "{\"ok\":true,\"result\":{\"uptime_s\":%lu,\"free_heap_bytes\":%d,"
             "\"wifi_connected\":%s}}",
             (unsigned long)uptime_s, heap_free, wifi_app_is_connected() ? "true" : "false");
    *output_length = strlen(output_json);
    return ESP_OK;
}

/* ---------------- agent 事件桥接（console 为主） ---------------- */

static const char *agent_event_name(otool_llm_agent_event_type_t t)
{
    switch (t) {
    case OTOOL_LLM_AGENT_EVENT_RUN_STARTED: return "RUN_STARTED";
    case OTOOL_LLM_AGENT_EVENT_TURN_STARTED: return "TURN_STARTED";
    case OTOOL_LLM_AGENT_EVENT_TEXT_DELTA: return "TEXT_DELTA";
    case OTOOL_LLM_AGENT_EVENT_TOOL_CALL_STARTED: return "TOOL_CALL_STARTED";
    case OTOOL_LLM_AGENT_EVENT_TOOL_ARGUMENTS_DELTA: return "TOOL_ARGUMENTS_DELTA";
    case OTOOL_LLM_AGENT_EVENT_TOOL_CALL_READY: return "TOOL_CALL_READY";
    case OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_STARTED: return "TOOL_EXECUTION_STARTED";
    case OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_FINISHED: return "TOOL_EXECUTION_FINISHED";
    case OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_FAILED: return "TOOL_EXECUTION_FAILED";
    case OTOOL_LLM_AGENT_EVENT_USAGE: return "USAGE";
    case OTOOL_LLM_AGENT_EVENT_TURN_COMPLETED: return "TURN_COMPLETED";
    case OTOOL_LLM_AGENT_EVENT_RUN_COMPLETED: return "RUN_COMPLETED";
    case OTOOL_LLM_AGENT_EVENT_RUN_LIMIT_REACHED: return "RUN_LIMIT_REACHED";
    case OTOOL_LLM_AGENT_EVENT_CANCELLED: return "CANCELLED";
    case OTOOL_LLM_AGENT_EVENT_ERROR: return "ERROR";
    default: return "?";
    }
}

static void reply_append(const char *data, size_t len)
{
    if (xSemaphoreTake(s_reply_lock, portMAX_DELAY) == pdTRUE) {
        size_t room = AGENT_REPLY_CAP - 1 - s_reply_len;
        if (len > room) {
            len = room;
        }
        memcpy(s_reply_buf + s_reply_len, data, len);
        s_reply_len += len;
        s_reply_buf[s_reply_len] = '\0';
        xSemaphoreGive(s_reply_lock);
    }
}

static void reply_reset(void)
{
    if (xSemaphoreTake(s_reply_lock, portMAX_DELAY) == pdTRUE) {
        s_reply_len = 0;
        s_reply_buf[0] = '\0';
        s_last_error[0] = '\0';
        xSemaphoreGive(s_reply_lock);
    }
}

static void set_error(const char *msg)
{
    if (xSemaphoreTake(s_reply_lock, portMAX_DELAY) == pdTRUE) {
        snprintf(s_last_error, sizeof(s_last_error), "%.127s", msg ? msg : "agent error");
        xSemaphoreGive(s_reply_lock);
    }
}

static otool_llm_event_action_t agent_on_event(const otool_llm_agent_event_t *evt, void *user_ctx)
{
    (void)user_ctx;
    printf("[agent] %s", agent_event_name(evt->type));
    switch (evt->type) {
    case OTOOL_LLM_AGENT_EVENT_TURN_STARTED:
        printf(" turn=%u", (unsigned)evt->turn_index);
        break;
    case OTOOL_LLM_AGENT_EVENT_TEXT_DELTA:
        printf(": %.*s", (int)evt->data.text_delta.data_len, evt->data.text_delta.data);
        reply_append(evt->data.text_delta.data, evt->data.text_delta.data_len);
        break;
    case OTOOL_LLM_AGENT_EVENT_TOOL_CALL_STARTED:
        printf(" name=%s", evt->name ? evt->name : "?");
        break;
    case OTOOL_LLM_AGENT_EVENT_TOOL_CALL_READY:
        printf(" args=%.*s", (int)evt->data.tool_call_ready.arguments_len,
               evt->data.tool_call_ready.arguments);
        break;
    case OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_STARTED:
        printf(" executing=%s", evt->name ? evt->name : "?");
        break;
    case OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_FINISHED:
        printf(" result=%.*s", (int)evt->data.tool_execution_finished.output_len,
               evt->data.tool_execution_finished.output);
        break;
    case OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_FAILED:
        printf(" failed: %.*s", (int)evt->data.tool_execution_finished.output_len,
               evt->data.tool_execution_finished.output);
        break;
    case OTOOL_LLM_AGENT_EVENT_USAGE:
        printf(" in=%lld out=%lld", (long long)evt->data.usage.input_tokens,
               (long long)evt->data.usage.output_tokens);
        break;
    case OTOOL_LLM_AGENT_EVENT_ERROR:
        printf(" code=0x%x", (unsigned)evt->data.error.code);
        set_error(evt->data.error.message);
        break;
    case OTOOL_LLM_AGENT_EVENT_RUN_LIMIT_REACHED:
        set_error("run limit reached");
        break;
    default:
        break;
    }
    printf("\n");
    fflush(stdout);
    return OTOOL_LLM_EVENT_ACTION_CONTINUE;
}

/* ---------------- worker ---------------- */

static void agent_worker_task(void *arg)
{
    (void)arg;

    if (wifi_app_wait_connected(30000) != ESP_OK) {
        ESP_LOGE(TAG, "wifi not connected, agent disabled");
        vTaskDelete(nullptr);
        return;
    }
    const char *api_key = credential_llm_key();
    if (api_key == nullptr || api_key[0] == '\0') {
        ESP_LOGE(TAG, "llm key not set; run 'cred-set llm_key <key>'");
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
    if (otool_llm_client_create(&cfg, &client) != ESP_OK) {
        ESP_LOGE(TAG, "client create failed");
        vTaskDelete(nullptr);
        return;
    }

    /* 工具 registry（seal 后 agent 使用） */
    otool_llm_tool_registry_handle_t reg = nullptr;
    if (otool_llm_tool_registry_create(&reg) != ESP_OK) {
        otool_llm_client_destroy(client);
        vTaskDelete(nullptr);
        return;
    }
    otool_llm_tool_definition_t tool = {};
    tool.struct_size = sizeof(tool);
    tool.name = "get_device_status";
    tool.description = "获取设备状态：运行时长、剩余堆内存、Wi-Fi 连接状态";
    tool.parameters_json_schema =
        "{\"type\":\"object\",\"properties\":{},\"required\":[],\"additionalProperties\":false}";
    tool.flags = OTOOL_LLM_TOOL_READ_ONLY;
    tool.timeout_ms = 5000;
    tool.execute = tool_get_device_status;
    if (otool_llm_tool_registry_add(reg, &tool) != ESP_OK) {
        ESP_LOGE(TAG, "tool register failed");
        otool_llm_tool_registry_destroy(reg);
        otool_llm_client_destroy(client);
        vTaskDelete(nullptr);
        return;
    }
    otool_llm_tool_registry_seal(reg);

    otool_llm_agent_config_t acfg = {};
    acfg.struct_size = sizeof(acfg);
    acfg.client = client;
    acfg.tools = reg;
    acfg.model = CONFIG_OTOOL_LLM_MODEL;
    acfg.instructions = "你是 Tab5 设备助手。回答使用中文。";
    acfg.state_mode = OTOOL_LLM_AGENT_STATE_REMOTE_RESPONSE_CHAIN;
    acfg.max_turns = 4;
    acfg.max_tool_calls = 4;
    acfg.run_timeout_ms = 120000;
    otool_llm_agent_handle_t agent = nullptr;
    if (otool_llm_agent_create(&acfg, &agent) != ESP_OK) {
        ESP_LOGE(TAG, "agent create failed");
        otool_llm_tool_registry_destroy(reg);
        otool_llm_client_destroy(client);
        vTaskDelete(nullptr);
        return;
    }

    char question[256] = { 0 };
    for (;;) {
        /* 等待触发（console agent <text> / agent-cancel） */
        xSemaphoreTake(s_trigger_sem, portMAX_DELAY);

        if (xSemaphoreTake(s_reply_lock, portMAX_DELAY) == pdTRUE) {
            snprintf(question, sizeof(question), "%s", s_pending_question);
            s_pending_question[0] = '\0';
            xSemaphoreGive(s_reply_lock);
        }

        /* 若上一轮还在跑，先取消（agent-cancel 或新请求打断） */
        if (s_busy) {
            otool_llm_agent_cancel(agent);
            int waited = 0;
            while (s_busy && waited++ < 300) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        if (question[0] == '\0') {
            continue; /* 仅取消，不启动新 run */
        }

        reply_reset();
        s_busy = true;
        s_round = s_round + 1;
        printf("[agent] ===== run %d: %s =====\n", s_round, question);
        fflush(stdout);

        esp_err_t err = otool_llm_agent_run_stream(agent, question, agent_on_event, nullptr);
        printf("[agent] run %d done: %s\n", s_round, esp_err_to_name(err));
        fflush(stdout);
        if (err != ESP_OK && err != OTOOL_LLM_ERR_AGENT_LIMIT) {
            set_error(esp_err_to_name(err));
        }
        s_busy = false;
    }
}

/* ---------------- 公共接口 ---------------- */

extern "C" void agent_app_ask(const char *text)
{
    if (text == nullptr || text[0] == '\0') {
        return;
    }
    if (xSemaphoreTake(s_reply_lock, portMAX_DELAY) == pdTRUE) {
        snprintf(s_pending_question, sizeof(s_pending_question), "%.255s", text);
        xSemaphoreGive(s_reply_lock);
    }
    if (s_trigger_sem != nullptr) {
        xSemaphoreGive(s_trigger_sem);
    }
}

extern "C" void agent_app_cancel(void)
{
    /* 由 worker 中的 agent_cancel 处理：worker 每轮循环开始时取消旧 run */
    if (s_trigger_sem != nullptr) {
        xSemaphoreGive(s_trigger_sem); /* 空问题触发 → 取消路径 */
    }
}

extern "C" void agent_app_status(char *buf, size_t cap)
{
    if (xSemaphoreTake(s_reply_lock, portMAX_DELAY) == pdTRUE) {
        snprintf(buf, cap, "round=%d busy=%d reply_len=%u err=%s", s_round, (int)s_busy,
                 (unsigned)s_reply_len, s_last_error[0] ? s_last_error : "-");
        xSemaphoreGive(s_reply_lock);
    } else {
        snprintf(buf, cap, "status unavailable");
    }
}

extern "C" size_t agent_app_reply_read(char *buf, size_t cap)
{
    if (buf == nullptr || cap == 0) {
        return 0;
    }
    size_t n = 0;
    if (xSemaphoreTake(s_reply_lock, portMAX_DELAY) == pdTRUE) {
        n = s_reply_len < cap - 1 ? s_reply_len : cap - 1;
        memcpy(buf, s_reply_buf, n);
        buf[n] = '\0';
        xSemaphoreGive(s_reply_lock);
    } else {
        buf[0] = '\0';
    }
    return n;
}

extern "C" void agent_app_start(void)
{
    s_trigger_sem = xSemaphoreCreateCounting(4, 0);
    s_reply_lock = xSemaphoreCreateMutex();

    BaseType_t created = xTaskCreatePinnedToCore(agent_worker_task, "agent_worker",
                                                 32768, nullptr, 5,
                                                 nullptr, 1);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "agent worker create failed");
    }
}
