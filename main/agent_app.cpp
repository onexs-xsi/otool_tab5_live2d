// Agent app: 设备工具 + Agent worker（WP6）。
// Agent 事件全部打印到 console（LLM 测试以 console 为主）；最终回复累积到
// 共享 buffer 供 UI 显示；注册只读状态工具和 policy-gated 可逆 UI 状态工具。

#include "agent_app.h"
#include "credential_store.h"
#include "wifi_app.h"

#include "otool_llm_agent.h"
#include "otool_llm_sdk.h"

#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

#include <atomic>
#include <cstdio>
#include <cstring>

static const char *TAG = "agent_app";

static void secure_zero_local(void *value, size_t length)
{
    volatile unsigned char *p = static_cast<volatile unsigned char *>(value);
    while (length-- > 0) {
        *p++ = 0;
    }
}

/* NVS：agent 协议选择（"responses" | "chat"），重启生效 */
#define AGENT_PROTO_NVS_NAMESPACE "otool_cfg"
#define AGENT_PROTO_NVS_KEY "agent_proto"

static const char *agent_proto_get(void)
{
    static char proto[16] = { 0 };
    nvs_handle_t h;
    if (nvs_open(AGENT_PROTO_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(proto);
        if (nvs_get_str(h, AGENT_PROTO_NVS_KEY, proto, &len) != ESP_OK) {
            proto[0] = '\0';
        }
        nvs_close(h);
    }
    if (proto[0] == '\0' || (strcmp(proto, "chat") != 0 && strcmp(proto, "responses") != 0)) {
        return "responses"; /* 默认 */
    }
    return proto;
}

static constexpr size_t AGENT_REPLY_CAP = 4096;

static SemaphoreHandle_t s_trigger_sem = nullptr;   /* agent <text> 触发 */
static SemaphoreHandle_t s_reply_lock = nullptr;    /* 回复/状态缓冲锁 */
static char s_pending_question[256] = { 0 };
static char s_reply_buf[AGENT_REPLY_CAP];
static size_t s_reply_len = 0;
static char s_last_error[192] = { 0 };
static char s_tool_ui_status[64] = "-";
static std::atomic_bool s_busy{false};
static std::atomic_int s_round{0};
static std::atomic<agent_app_phase_t> s_phase{AGENT_APP_PHASE_BOOTING};
static std::atomic_bool s_cancel_pending{false}; /* run 未开始时丢弃排队问题 */
static std::atomic_bool s_reset_pending{false};
/* agent handle 跨 task 暴露：console 的 agent-cancel 直接中断进行中的 run */
static std::atomic<otool_llm_agent_handle_t> s_agent{nullptr};

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
    int written = snprintf(output_json, output_capacity,
                           "{\"ok\":true,\"result\":{\"uptime_s\":%lu,"
                           "\"free_heap_bytes\":%d,\"wifi_connected\":%s}}",
                           (unsigned long)uptime_s, heap_free,
                           wifi_app_is_connected() ? "true" : "false");
    if (written < 0 || (size_t)written >= output_capacity) {
        return OTOOL_LLM_ERR_TOOL_OUTPUT_TOO_LARGE;
    }
    *output_length = (size_t)written;
    return ESP_OK;
}

/* Reversible app-owned side effect: changes only the short status text shown by this demo. */
static esp_err_t tool_set_ui_status(const char *arguments_json, char *output_json,
                                    size_t output_capacity, size_t *output_length,
                                    const otool_llm_tool_exec_context_t *exec_ctx,
                                    void *user_ctx)
{
    (void)exec_ctx;
    (void)user_ctx;
    cJSON *args = cJSON_Parse(arguments_json);
    cJSON *status = args != nullptr ? cJSON_GetObjectItemCaseSensitive(args, "status") : nullptr;
    if (!cJSON_IsString(status) || status->valuestring == nullptr ||
        strlen(status->valuestring) > 48) {
        cJSON_Delete(args);
        return OTOOL_LLM_ERR_TOOL_ARGUMENTS;
    }
    if (xSemaphoreTake(s_reply_lock, portMAX_DELAY) != pdTRUE) {
        cJSON_Delete(args);
        return ESP_FAIL;
    }
    snprintf(s_tool_ui_status, sizeof(s_tool_ui_status), "%s", status->valuestring);
    xSemaphoreGive(s_reply_lock);

    cJSON *result = cJSON_CreateObject();
    cJSON *payload = cJSON_CreateObject();
    bool payload_owned = false;
    bool ok = result != nullptr && payload != nullptr &&
              cJSON_AddBoolToObject(result, "ok", true) != nullptr;
    if (ok) {
        payload_owned = cJSON_AddItemToObject(result, "result", payload);
        ok = payload_owned &&
             cJSON_AddStringToObject(payload, "status", status->valuestring) != nullptr &&
             cJSON_PrintPreallocated(result, output_json, (int)output_capacity, false);
    }
    cJSON_Delete(result);
    if (!ok) {
        if (!payload_owned) {
            cJSON_Delete(payload);
        }
        cJSON_Delete(args);
        return output_capacity < 64 ? OTOOL_LLM_ERR_TOOL_OUTPUT_TOO_LARGE : ESP_ERR_NO_MEM;
    }
    *output_length = strlen(output_json);
    cJSON_Delete(args);
    return ESP_OK;
}

static otool_llm_tool_decision_t agent_tool_policy(const char *tool_name,
                                                    const char *arguments_json,
                                                    uint32_t tool_flags, void *user_ctx)
{
    (void)arguments_json;
    (void)tool_flags;
    (void)user_ctx;
    return strcmp(tool_name, "set_ui_status") == 0 ? OTOOL_LLM_TOOL_DECISION_ALLOW
                                                    : OTOOL_LLM_TOOL_DECISION_DENY;
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
    case OTOOL_LLM_AGENT_EVENT_RUN_STARTED:
    case OTOOL_LLM_AGENT_EVENT_TURN_STARTED:
        s_phase.store(AGENT_APP_PHASE_THINKING);
        if (evt->type != OTOOL_LLM_AGENT_EVENT_TURN_STARTED) {
            break;
        }
        printf(" turn=%u", (unsigned)evt->turn_index);
        break;
    case OTOOL_LLM_AGENT_EVENT_TEXT_DELTA:
        s_phase.store(AGENT_APP_PHASE_RESPONDING);
        printf(": %.*s", (int)evt->data.text_delta.data_len, evt->data.text_delta.data);
        reply_append(evt->data.text_delta.data, evt->data.text_delta.data_len);
        break;
    case OTOOL_LLM_AGENT_EVENT_TOOL_CALL_STARTED:
    case OTOOL_LLM_AGENT_EVENT_TOOL_ARGUMENTS_DELTA:
    case OTOOL_LLM_AGENT_EVENT_TOOL_CALL_READY:
    case OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_STARTED:
        s_phase.store(AGENT_APP_PHASE_TOOL);
        if (evt->type == OTOOL_LLM_AGENT_EVENT_TOOL_CALL_STARTED) {
            printf(" name=%s", evt->name ? evt->name : "?");
        } else if (evt->type == OTOOL_LLM_AGENT_EVENT_TOOL_CALL_READY) {
            printf(" args_bytes=%u", (unsigned)evt->data.tool_call_ready.arguments_len);
        } else if (evt->type == OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_STARTED) {
            printf(" executing=%s", evt->name ? evt->name : "?");
        }
        break;
    case OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_FINISHED:
        s_phase.store(AGENT_APP_PHASE_THINKING);
        printf(" result_bytes=%u", (unsigned)evt->data.tool_execution_finished.output_len);
        break;
    case OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_FAILED:
        s_phase.store(AGENT_APP_PHASE_ERROR);
        printf(" failed result_bytes=%u",
               (unsigned)evt->data.tool_execution_finished.output_len);
        break;
    case OTOOL_LLM_AGENT_EVENT_USAGE:
        printf(" in=%lld out=%lld", (long long)evt->data.usage.input_tokens,
               (long long)evt->data.usage.output_tokens);
        break;
    case OTOOL_LLM_AGENT_EVENT_ERROR:
        s_phase.store(AGENT_APP_PHASE_ERROR);
        printf(" code=0x%x msg=%.100s", (unsigned)evt->data.error.code,
               evt->data.error.message != nullptr ? evt->data.error.message : "");
        set_error(evt->data.error.message);
        break;
    case OTOOL_LLM_AGENT_EVENT_RUN_LIMIT_REACHED:
        s_phase.store(AGENT_APP_PHASE_ERROR);
        set_error("run limit reached");
        break;
    case OTOOL_LLM_AGENT_EVENT_RUN_COMPLETED:
        s_phase.store(AGENT_APP_PHASE_COMPLETED);
        break;
    case OTOOL_LLM_AGENT_EVENT_CANCELLED:
        s_phase.store(AGENT_APP_PHASE_CANCELLED);
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

    /* Missing API key is a supported disabled state. Check it before Wi-Fi so
     * offline boot reports the actionable configuration immediately. */
    char api_key[256] = { 0 };
    esp_err_t credential_err =
        credential_store_copy_runtime("llm_key", api_key, sizeof(api_key));
    if (credential_err != ESP_OK || api_key[0] == '\0') {
        secure_zero_local(api_key, sizeof(api_key));
        set_error("Agent disabled: CONFIG_OTOOL_LLM_API_KEY is empty");
        s_phase.store(AGENT_APP_PHASE_DISABLED);
        ESP_LOGW(TAG, "Agent disabled: set CONFIG_OTOOL_LLM_API_KEY in sdkconfig and rebuild");
        vTaskDelete(nullptr);
        return;
    }
    secure_zero_local(api_key, sizeof(api_key));

    /* wifi 可能因链路问题（SDIO/供电）暂时不可用：循环等待，恢复后 agent 自动可用。
     * 注意：不能超时退出——否则之后所有 agent 命令无人处理。 */
    s_phase.store(AGENT_APP_PHASE_CONNECTING);
    while (wifi_app_wait_connected(30000) != ESP_OK) {
        ESP_LOGW(TAG, "wifi not connected yet, agent worker waiting...");
    }
    credential_err = credential_store_copy_runtime("llm_key", api_key, sizeof(api_key));
    if (credential_err != ESP_OK || api_key[0] == '\0') {
        secure_zero_local(api_key, sizeof(api_key));
        set_error("Agent disabled: API key became unavailable");
        s_phase.store(AGENT_APP_PHASE_DISABLED);
        ESP_LOGW(TAG, "Agent disabled: effective API key became unavailable");
        vTaskDelete(nullptr);
        return;
    }
    otool_llm_client_config_t cfg = {};
    cfg.struct_size = sizeof(cfg);
    cfg.provider = OTOOL_LLM_PROVIDER_VOLCENGINE_ARK;
    cfg.api_key = api_key;
    cfg.connect_timeout_ms = 10000;
    cfg.read_timeout_ms = 60000;
    const char *proto = agent_proto_get();
    if (strcmp(proto, "chat") == 0) {
        cfg.protocol = OTOOL_LLM_PROTOCOL_CHAT_COMPLETIONS_SSE;
        ESP_LOGI(TAG, "agent protocol: Chat Completions (local transcript)");
    } else {
        cfg.protocol = OTOOL_LLM_PROTOCOL_AUTO;
        ESP_LOGI(TAG, "agent protocol: Responses (remote chain)");
    }
    otool_llm_client_handle_t client = nullptr;
    esp_err_t client_err = otool_llm_client_create(&cfg, &client);
    secure_zero_local(api_key, sizeof(api_key));
    if (client_err != ESP_OK) {
        s_phase.store(AGENT_APP_PHASE_ERROR);
        ESP_LOGE(TAG, "client create failed");
        vTaskDelete(nullptr);
        return;
    }

    /* 工具 registry（seal 后 agent 使用） */
    otool_llm_tool_registry_handle_t reg = nullptr;
    if (otool_llm_tool_registry_create(&reg) != ESP_OK) {
        s_phase.store(AGENT_APP_PHASE_ERROR);
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
    tool.strict = true;
    tool.flags = OTOOL_LLM_TOOL_READ_ONLY;
    tool.timeout_ms = 5000;
    tool.max_output_bytes = 512;
    tool.execute = tool_get_device_status;
    if (otool_llm_tool_registry_add(reg, &tool) != ESP_OK) {
        s_phase.store(AGENT_APP_PHASE_ERROR);
        ESP_LOGE(TAG, "tool register failed");
        otool_llm_tool_registry_destroy(reg);
        otool_llm_client_destroy(client);
        vTaskDelete(nullptr);
        return;
    }
    otool_llm_tool_definition_t ui_tool = {};
    ui_tool.struct_size = sizeof(ui_tool);
    ui_tool.name = "set_ui_status";
    ui_tool.description = "设置设备界面上的短状态文字；仅影响本应用状态栏，可重复覆盖";
    ui_tool.parameters_json_schema =
        "{\"type\":\"object\",\"properties\":{\"status\":{\"type\":\"string\"}},"
        "\"required\":[\"status\"],\"additionalProperties\":false}";
    ui_tool.strict = true;
    ui_tool.flags = OTOOL_LLM_TOOL_SIDE_EFFECTING | OTOOL_LLM_TOOL_IDEMPOTENT;
    ui_tool.timeout_ms = 1000;
    ui_tool.max_output_bytes = 512;
    ui_tool.execute = tool_set_ui_status;
    if (otool_llm_tool_registry_add(reg, &ui_tool) != ESP_OK) {
        s_phase.store(AGENT_APP_PHASE_ERROR);
        ESP_LOGE(TAG, "set_ui_status tool register failed");
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
    acfg.instructions = "你是 Tab5 设备助手。回答使用中文；需要时可读取设备状态或更新界面短状态。";
    acfg.state_mode = strcmp(proto, "chat") == 0 ? OTOOL_LLM_AGENT_STATE_LOCAL_TRANSCRIPT
                                                : OTOOL_LLM_AGENT_STATE_REMOTE_RESPONSE_CHAIN;
    acfg.max_turns = 4;
    acfg.max_tool_calls = 4;
    acfg.run_timeout_ms = 120000;
    acfg.policy = agent_tool_policy;
    otool_llm_agent_handle_t agent = nullptr;
    if (otool_llm_agent_create(&acfg, &agent) != ESP_OK) {
        s_phase.store(AGENT_APP_PHASE_ERROR);
        ESP_LOGE(TAG, "agent create failed");
        otool_llm_tool_registry_destroy(reg);
        otool_llm_client_destroy(client);
        vTaskDelete(nullptr);
        return;
    }
    s_agent.store(agent); /* console agent-cancel 可用 */
    s_phase.store(AGENT_APP_PHASE_READY);

    char question[256] = { 0 };
    for (;;) {
        /* 等待触发（console agent <text> / agent-cancel） */
        xSemaphoreTake(s_trigger_sem, portMAX_DELAY);

        if (xSemaphoreTake(s_reply_lock, portMAX_DELAY) == pdTRUE) {
            snprintf(question, sizeof(question), "%s", s_pending_question);
            s_pending_question[0] = '\0';
            xSemaphoreGive(s_reply_lock);
        }

        /* 排队取消：agent-cancel 在 run 开始前发出时，丢弃排队中的问题 */
        if (s_cancel_pending.exchange(false)) {
            question[0] = '\0';
        }

        if (s_reset_pending.exchange(false)) {
            otool_llm_agent_reset_session(agent);
            reply_reset();
            if (xSemaphoreTake(s_reply_lock, portMAX_DELAY) == pdTRUE) {
                snprintf(s_tool_ui_status, sizeof(s_tool_ui_status), "-");
                xSemaphoreGive(s_reply_lock);
            }
            s_phase.store(AGENT_APP_PHASE_READY);
            ESP_LOGI(TAG, "agent conversation reset");
        }
        if (question[0] == '\0') {
            continue; /* 仅取消，不启动新 run */
        }

        reply_reset();
        s_busy.store(true);
        s_phase.store(AGENT_APP_PHASE_THINKING);
        int round = s_round.fetch_add(1) + 1;
        printf("[agent] ===== run %d: question_bytes=%u =====\n", round,
               (unsigned)strlen(question));
        fflush(stdout);

        esp_err_t err = otool_llm_agent_run_stream(agent, question, agent_on_event, nullptr);
        printf("[agent] run %d done: %s\n", round, esp_err_to_name(err));
        fflush(stdout);
        if (err != ESP_OK && err != OTOOL_LLM_ERR_AGENT_LIMIT) {
            set_error(esp_err_to_name(err));
            if (s_phase.load() != AGENT_APP_PHASE_CANCELLED) {
                s_phase.store(AGENT_APP_PHASE_ERROR);
            }
        } else if (err == ESP_OK) {
            s_phase.store(AGENT_APP_PHASE_COMPLETED);
        }
        s_busy.store(false);
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
    /* A newer question supersedes an older cancel-only wakeup. This preserves
     * the UI's cancel-then-ask replacement sequence. */
    s_cancel_pending.store(false);
    /* `agent <new text>` replaces the active run while preserving the new
     * queued question. The worker consumes it after cancellation returns. */
    otool_llm_agent_handle_t h = s_agent.load();
    if (h != nullptr && s_busy.load()) {
        otool_llm_agent_cancel(h);
    }
    if (s_trigger_sem != nullptr) {
        xSemaphoreGive(s_trigger_sem);
    }
}

extern "C" void agent_app_cancel(void)
{
    /* 直接中断进行中的 run（otool_llm_agent_cancel 线程安全：置标志 +
     * 关闭活动请求 socket → run_stream 在下一事件循环返回并 emit CANCELLED）；
     * run 尚未开始时（worker 排队/等待 wifi），置 pending 由 worker 丢弃排队问题。 */
    s_cancel_pending.store(true);
    otool_llm_agent_handle_t h = s_agent.load();
    if (h != nullptr) {
        otool_llm_agent_cancel(h);
    }
    if (s_trigger_sem != nullptr) {
        xSemaphoreGive(s_trigger_sem);
    }
}

extern "C" void agent_app_reset_session(void)
{
    s_reset_pending.store(true);
    s_cancel_pending.store(true);
    otool_llm_agent_handle_t h = s_agent.load();
    if (h != nullptr) {
        otool_llm_agent_cancel(h);
    }
    if (s_trigger_sem != nullptr) {
        xSemaphoreGive(s_trigger_sem);
    }
}

extern "C" void agent_app_status(char *buf, size_t cap)
{
    if (xSemaphoreTake(s_reply_lock, portMAX_DELAY) == pdTRUE) {
        snprintf(buf, cap, "round=%d busy=%d reply_len=%u ui=%.48s err=%s", s_round.load(),
                 (int)s_busy.load(), (unsigned)s_reply_len, s_tool_ui_status,
                 s_last_error[0] ? s_last_error : "-");
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

extern "C" const char *agent_proto_name(void)
{
    return agent_proto_get();
}

extern "C" agent_app_phase_t agent_app_phase(void)
{
    return s_phase.load();
}

extern "C" void agent_app_start(void)
{
    s_trigger_sem = xSemaphoreCreateCounting(4, 0);
    s_reply_lock = xSemaphoreCreateMutex();

    BaseType_t created = xTaskCreatePinnedToCore(agent_worker_task, "agent_worker",
                                                 CONFIG_OTOOL_LLM_AGENT_TASK_STACK_SIZE, nullptr, 5,
                                                 nullptr, 1);
    if (created != pdPASS) {
        s_phase.store(AGENT_APP_PHASE_ERROR);
        ESP_LOGE(TAG, "agent worker create failed");
    }
}
