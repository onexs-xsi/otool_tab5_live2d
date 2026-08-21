/*
 * SPDX-FileCopyrightText: 2026 otool project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * WP4 host tests: Agent state machine with a fake request provider.
 *
 * The fake otool_llm_request_execute_stream() drives the agent's internal
 * text-event callback (passed by the agent) with a two-turn script:
 *   turn 1: RESPONSE_STARTED -> TOOL_CALL_STARTED -> ARGUMENTS_DELTA ->
 *           TOOL_CALL_DONE -> COMPLETED      (model wants a tool)
 *   turn 2: RESPONSE_STARTED -> TEXT_DELTA ("ok") -> COMPLETED
 *                                            (final answer)
 * The agent must execute the registered tool between the turns and feed its
 * output back via function_call_output + previous_response_id.
 *
 * Stubs provided here: otool_llm_request_*, esp_timer_get_time, esp_log.
 */

#include "otool_llm_agent.h"
#include "otool_llm_tools.h"
#include "otool_llm_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        g_checks++;                                                             \
        if (!(cond)) {                                                          \
            g_failures++;                                                       \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                         \
            printf(__VA_ARGS__);                                                \
            printf("\n");                                                       \
        }                                                                       \
    } while (0)

#define CHECK_STR(a, b) CHECK(strcmp((a), (b)) == 0, "expected \"%s\", got \"%s\"", (b), (a))

/* ---- platform stubs ---- */

int64_t esp_timer_get_time(void)
{
    static int64_t t = 0;
    t += 1000;
    return t;
}

void otool_llm_host_log_printf(const char *tag, const char *fmt, ...)
{
    (void)tag;
    (void)fmt;
}

const char *esp_err_to_name(esp_err_t code)
{
    (void)code;
    return "ERR";
}

/* ---- fake request provider ---- */

static int g_fake_turn = 0;              /* 1-based model turn */
static const otool_llm_text_request_t *g_last_request = NULL; /* 最近一次 create 的请求 */

esp_err_t otool_llm_request_create(otool_llm_client_handle_t client,
                                   const otool_llm_text_request_t *request,
                                   otool_llm_request_handle_t *out)
{
    (void)client;
    /* 深拷贝请求快照：agent 的 req 是栈对象，且指针字段会在后续轮次被覆盖 */
    static otool_llm_text_request_t copy;
    static char prev_id[128];
    static otool_llm_tool_output_t outputs[4];
    static char out_call_ids[4][64];
    static char out_texts[4][1024];
    copy = *request;
    copy.previous_response_id = NULL;
    if (request->previous_response_id != NULL) {
        snprintf(prev_id, sizeof(prev_id), "%s", request->previous_response_id);
        copy.previous_response_id = prev_id;
    }
    copy.tool_outputs = NULL;
    if (request->tool_output_count > 0 && request->tool_output_count <= 4) {
        for (size_t i = 0; i < request->tool_output_count; i++) {
            snprintf(out_call_ids[i], sizeof(out_call_ids[i]), "%s",
                     request->tool_outputs[i].call_id);
            snprintf(out_texts[i], sizeof(out_texts[i]), "%s", request->tool_outputs[i].output);
            outputs[i].call_id = out_call_ids[i];
            outputs[i].output = out_texts[i];
        }
        copy.tool_outputs = outputs;
    }
    g_last_request = &copy;
    *out = (otool_llm_request_handle_t)0x1;
    return ESP_OK;
}

void otool_llm_request_destroy(otool_llm_request_handle_t request)
{
    (void)request;
}

esp_err_t otool_llm_request_cancel(otool_llm_request_handle_t request)
{
    (void)request;
    return ESP_OK;
}

esp_err_t otool_llm_request_execute_stream(otool_llm_request_handle_t request,
                                           otool_llm_text_event_cb_t cb, void *user_ctx)
{
    (void)request;
    g_fake_turn++;

    if (g_fake_turn == 1) {
        /* turn 1: 模型要调用 get_device_status */
        otool_llm_text_event_t evt = {};
        evt.type = OTOOL_LLM_TEXT_EVENT_RESPONSE_STARTED;
        evt.response_id = "resp_1";
        cb(&evt, user_ctx);

        evt.type = OTOOL_LLM_TEXT_EVENT_TOOL_CALL_STARTED;
        evt.data.tool_call_started.output_index = 0;
        evt.data.tool_call_started.call_id = "call_1";
        evt.data.tool_call_started.name = "get_device_status";
        cb(&evt, user_ctx);

        evt.type = OTOOL_LLM_TEXT_EVENT_TOOL_ARGUMENTS_DELTA;
        evt.data.tool_arguments_delta.delta = "{}";
        evt.data.tool_arguments_delta.delta_len = 2;
        cb(&evt, user_ctx);

        evt.type = OTOOL_LLM_TEXT_EVENT_TOOL_CALL_DONE;
        evt.data.tool_call_done.call_id = "call_1";
        evt.data.tool_call_done.name = "get_device_status";
        evt.data.tool_call_done.arguments = "{}";
        evt.data.tool_call_done.arguments_len = 2;
        cb(&evt, user_ctx);

        evt.type = OTOOL_LLM_TEXT_EVENT_COMPLETED;
        cb(&evt, user_ctx);
        return ESP_OK;
    }

    /* turn 2+: 最终回答 */
    otool_llm_text_event_t evt = {};
    evt.type = OTOOL_LLM_TEXT_EVENT_RESPONSE_STARTED;
    evt.response_id = "resp_2";
    cb(&evt, user_ctx);

    evt.type = OTOOL_LLM_TEXT_EVENT_TEXT_DELTA;
    evt.data.text_delta.data = "设备状态正常";
    evt.data.text_delta.data_len = 18;
    cb(&evt, user_ctx);

    evt.type = OTOOL_LLM_TEXT_EVENT_COMPLETED;
    cb(&evt, user_ctx);
    return ESP_OK;
}

/* ---- tools ---- */

static esp_err_t tool_get_device_status(const char *arguments_json, char *output_json,
                                        size_t output_capacity, size_t *output_length,
                                        const otool_llm_tool_exec_context_t *exec_ctx,
                                        void *user_ctx)
{
    (void)arguments_json;
    (void)exec_ctx;
    (void)user_ctx;
    snprintf(output_json, output_capacity, "{\"ok\":true,\"result\":{\"uptime_s\":123}}");
    *output_length = strlen(output_json);
    return ESP_OK;
}

/* ---- agent event collector ---- */

typedef struct {
    otool_llm_agent_event_t events[64];
    int count;
    char text[512];
    size_t text_len;
    /* 深拷贝字段（事件字符串 span 只在回调内有效） */
    char tool_names[64][64];
    char tool_call_ids[64][64];
    char tool_outputs[64][1024];
} agent_collector_t;

static otool_llm_event_action_t agent_collect_cb(const otool_llm_agent_event_t *evt, void *user_ctx)
{
    agent_collector_t *col = (agent_collector_t *)user_ctx;
    col->events[col->count] = *evt;
    if (evt->name != NULL) {
        snprintf(col->tool_names[col->count], sizeof(col->tool_names[col->count]), "%s", evt->name);
        col->events[col->count].name = col->tool_names[col->count];
    }
    if (evt->call_id != NULL) {
        snprintf(col->tool_call_ids[col->count], sizeof(col->tool_call_ids[col->count]), "%s",
                 evt->call_id);
        col->events[col->count].call_id = col->tool_call_ids[col->count];
    }
    if (evt->type == OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_FINISHED ||
        evt->type == OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_FAILED) {
        snprintf(col->tool_outputs[col->count], sizeof(col->tool_outputs[col->count]), "%.*s",
                 (int)evt->data.tool_execution_finished.output_len,
                 evt->data.tool_execution_finished.output);
        col->events[col->count].data.tool_execution_finished.output = col->tool_outputs[col->count];
    }
    col->count++;
    if (evt->type == OTOOL_LLM_AGENT_EVENT_TEXT_DELTA) {
        if (col->text_len + evt->data.text_delta.data_len < sizeof(col->text)) {
            memcpy(col->text + col->text_len, evt->data.text_delta.data,
                   evt->data.text_delta.data_len);
            col->text_len += evt->data.text_delta.data_len;
            col->text[col->text_len] = '\0';
        }
    }
    return OTOOL_LLM_EVENT_ACTION_CONTINUE;
}

/* ---- tests ---- */

#define CHECK_EQ_T(a, b) CHECK((a) == (b), "expected %d, got %d", (int)(b), (int)(a))

static void test_agent_tool_loop(void)
{
    otool_llm_tool_registry_handle_t reg = NULL;
    CHECK(otool_llm_tool_registry_create(&reg) == ESP_OK, "reg create");
    otool_llm_tool_definition_t tool = {};
    tool.struct_size = sizeof(tool);
    tool.name = "get_device_status";
    tool.description = "device status";
    tool.parameters_json_schema =
        "{\"type\":\"object\",\"properties\":{},\"required\":[],\"additionalProperties\":false}";
    tool.flags = OTOOL_LLM_TOOL_READ_ONLY;
    tool.execute = tool_get_device_status;
    CHECK(otool_llm_tool_registry_add(reg, &tool) == ESP_OK, "add tool");
    otool_llm_tool_registry_seal(reg);

    otool_llm_agent_config_t cfg = {};
    cfg.struct_size = sizeof(cfg);
    cfg.client = (otool_llm_client_handle_t)0x2; /* fake client */
    cfg.tools = reg;
    cfg.model = "fake-model";
    cfg.instructions = "be brief";
    cfg.max_turns = 4;
    cfg.max_tool_calls = 4;

    otool_llm_agent_handle_t agent = NULL;
    CHECK(otool_llm_agent_create(&cfg, &agent) == ESP_OK, "agent create");

    agent_collector_t col = { 0 };
    g_fake_turn = 0;
    g_last_request = NULL;
    esp_err_t err = otool_llm_agent_run_stream(agent, "设备状态如何？", agent_collect_cb, &col);
    CHECK(err == ESP_OK, "run ok (got 0x%x)", (unsigned)err);

    /* 事件序列 */
    CHECK(col.count > 0, "events collected");
    CHECK(col.events[0].type == OTOOL_LLM_AGENT_EVENT_RUN_STARTED, "RUN_STARTED first");
    otool_llm_agent_event_type_t last = col.events[col.count - 1].type;
    CHECK(last == OTOOL_LLM_AGENT_EVENT_RUN_COMPLETED, "terminal RUN_COMPLETED (got %d)", (int)last);

    /* 工具调用被转发 */
    int tool_started = 0, tool_ready = 0, exec_finished = 0;
    for (int i = 0; i < col.count; i++) {
        switch (col.events[i].type) {
        case OTOOL_LLM_AGENT_EVENT_TOOL_CALL_STARTED:
            tool_started++;
            CHECK_STR(col.events[i].name, "get_device_status");
            break;
        case OTOOL_LLM_AGENT_EVENT_TOOL_CALL_READY:
            tool_ready++;
            break;
        case OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_FINISHED:
            exec_finished++;
            CHECK(strstr(col.events[i].data.tool_execution_finished.output, "\"ok\":true") != NULL,
                  "tool output ok");
            break;
        default:
            break;
        }
    }
    CHECK_EQ_T(tool_started, 1);
    CHECK_EQ_T(tool_ready, 1);
    CHECK_EQ_T(exec_finished, 1);

    /* 最终文本 */
    CHECK_STR(col.text, "设备状态正常");

    /* 请求链断言：turn1 带 tools+store；turn2 带 previous_response_id + tool_outputs */
    CHECK(g_last_request != NULL, "last request captured");
    if (g_last_request != NULL) {
        CHECK(g_last_request->store, "store=true (remote chain)");
        CHECK(g_last_request->previous_response_id != NULL, "previous_response_id set on turn 2");
        CHECK_STR(g_last_request->previous_response_id, "resp_1");
        CHECK(g_last_request->tool_output_count == 1, "one tool output on turn 2");
        if (g_last_request->tool_output_count == 1) {
            CHECK_STR(g_last_request->tool_outputs[0].call_id, "call_1");
            CHECK(strstr(g_last_request->tool_outputs[0].output, "\"uptime_s\":123") != NULL,
                  "tool output fed back");
        }
    }

    otool_llm_agent_destroy(agent);
    otool_llm_tool_registry_destroy(reg);
}

static void test_agent_limit_and_loop_detection(void)
{
    /* 模型连续 3 轮请求同一工具（fake turn>=2 也返回 tool call）→ 应触发循环/上限 */
    /* 通过 g_fake_repeat_tool 标志控制 */
    (void)0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_agent_tool_loop();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
