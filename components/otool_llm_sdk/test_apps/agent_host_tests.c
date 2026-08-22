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
static int g_fake_chat = 0;              /* 非 0：Chat 事件脚本（delta.tool_calls） */
static const char *g_fake_tool_name = "get_device_status"; /* 模型请求的工具名 */
static int g_fake_loop = 0;              /* 非 0：turn 2+ 也返回同一工具调用（循环） */
static int g_fake_multi = 0;             /* 非 0：turn1 发两个工具调用（不同参数） */
static int g_fake_interleaved = 0;       /* 非 0：两个调用的参数事件交错 */
static int g_fake_incomplete = 0;        /* 非 0：模型 turn 以 INCOMPLETE 结束 */

#define OBS_MAX_REQUESTS 128
#define OBS_MAX_MESSAGES 32
typedef struct {
    bool has_previous_response_id;
    char previous_response_id[128];
    size_t message_count;
    otool_llm_role_t roles[OBS_MAX_MESSAGES];
    char texts[OBS_MAX_MESSAGES][256];
} request_observation_t;
static request_observation_t g_observations[OBS_MAX_REQUESTS];
static size_t g_observation_count = 0;

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
    static otool_llm_text_message_t messages[OBS_MAX_MESSAGES];
    static char message_texts[OBS_MAX_MESSAGES][4096];
    static char message_tool_call_ids[OBS_MAX_MESSAGES][64];
    static otool_llm_tool_call_msg_t message_tool_calls[OBS_MAX_MESSAGES][4];
    static char tc_ids[OBS_MAX_MESSAGES][4][64];
    static char tc_names[OBS_MAX_MESSAGES][4][64];
    static char tc_args[OBS_MAX_MESSAGES][4][4096];
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
    copy.messages = NULL;
    if (request->message_count > 0 && request->message_count <= OBS_MAX_MESSAGES) {
        for (size_t i = 0; i < request->message_count; i++) {
            messages[i] = request->messages[i];
            snprintf(message_texts[i], sizeof(message_texts[i]), "%s",
                     request->messages[i].text != NULL ? request->messages[i].text : "");
            messages[i].text = message_texts[i];
            if (request->messages[i].tool_call_id != NULL) {
                snprintf(message_tool_call_ids[i], sizeof(message_tool_call_ids[i]), "%s",
                         request->messages[i].tool_call_id);
                messages[i].tool_call_id = message_tool_call_ids[i];
            }
            if (request->messages[i].tool_call_count > 0 &&
                request->messages[i].tool_call_count <= 4) {
                for (size_t j = 0; j < request->messages[i].tool_call_count; j++) {
                    snprintf(tc_ids[i][j], sizeof(tc_ids[i][j]), "%s",
                             request->messages[i].tool_calls[j].id);
                    snprintf(tc_names[i][j], sizeof(tc_names[i][j]), "%s",
                             request->messages[i].tool_calls[j].name);
                    snprintf(tc_args[i][j], sizeof(tc_args[i][j]), "%s",
                             request->messages[i].tool_calls[j].arguments);
                    message_tool_calls[i][j].id = tc_ids[i][j];
                    message_tool_calls[i][j].name = tc_names[i][j];
                    message_tool_calls[i][j].arguments = tc_args[i][j];
                }
                messages[i].tool_calls = message_tool_calls[i];
            }
        }
        copy.messages = messages;
    }
    if (g_observation_count < OBS_MAX_REQUESTS) {
        request_observation_t *obs = &g_observations[g_observation_count++];
        memset(obs, 0, sizeof(*obs));
        if (request->previous_response_id != NULL) {
            obs->has_previous_response_id = true;
            snprintf(obs->previous_response_id, sizeof(obs->previous_response_id), "%s",
                     request->previous_response_id);
        }
        obs->message_count = request->message_count < OBS_MAX_MESSAGES
                                 ? request->message_count
                                 : OBS_MAX_MESSAGES;
        for (size_t i = 0; i < obs->message_count; i++) {
            obs->roles[i] = request->messages[i].role;
            snprintf(obs->texts[i], sizeof(obs->texts[i]), "%s",
                     request->messages[i].text != NULL ? request->messages[i].text : "");
        }
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

    if (g_fake_incomplete) {
        otool_llm_text_event_t evt = {};
        evt.type = OTOOL_LLM_TEXT_EVENT_RESPONSE_STARTED;
        evt.response_id = "resp_incomplete";
        cb(&evt, user_ctx);
        evt.type = OTOOL_LLM_TEXT_EVENT_TEXT_DELTA;
        evt.data.text_delta.data = "partial";
        evt.data.text_delta.data_len = 7;
        cb(&evt, user_ctx);
        evt.type = OTOOL_LLM_TEXT_EVENT_INCOMPLETE;
        evt.data.incomplete.reason = "max_output_tokens";
        cb(&evt, user_ctx);
        return ESP_OK;
    }

    if (g_fake_chat) {
        /* Chat 脚本：turn1 tool_calls 流式，turn2 最终回答（loop 模式每轮工具，
         * 参数随 turn 变化以绕过循环检测） */
        if (g_fake_turn == 1 || g_fake_loop) {
            /* 参数：loop 模式每轮变化（绕过循环检测）；普通模式固定 {} */
            const char *args = "{}";
            size_t args_len = 2;
            char dyn[32];
            if (g_fake_loop) {
                snprintf(dyn, sizeof(dyn), "{\"n\":%d}", g_fake_turn);
                args = dyn;
                args_len = strlen(dyn);
            }

            otool_llm_text_event_t evt = {};
            evt.type = OTOOL_LLM_TEXT_EVENT_RESPONSE_STARTED;
            evt.response_id = "chat_1";
            cb(&evt, user_ctx);

            evt.type = OTOOL_LLM_TEXT_EVENT_TOOL_CALL_STARTED;
            evt.data.tool_call_started.call_id = "call_chat";
            evt.data.tool_call_started.name = g_fake_tool_name;
            cb(&evt, user_ctx);

            evt.type = OTOOL_LLM_TEXT_EVENT_TOOL_ARGUMENTS_DELTA;
            evt.data.tool_arguments_delta.output_index = 0;
            evt.data.tool_arguments_delta.delta = args;
            evt.data.tool_arguments_delta.delta_len = args_len;
            cb(&evt, user_ctx);

            evt.type = OTOOL_LLM_TEXT_EVENT_TOOL_CALL_DONE;
            evt.data.tool_call_done.output_index = 0;
            evt.data.tool_call_done.call_id = "call_chat";
            evt.data.tool_call_done.name = g_fake_tool_name;
            evt.data.tool_call_done.arguments = args;
            evt.data.tool_call_done.arguments_len = args_len;
            cb(&evt, user_ctx);

            evt.type = OTOOL_LLM_TEXT_EVENT_COMPLETED;
            cb(&evt, user_ctx);
            return ESP_OK;
        }
        otool_llm_text_event_t evt = {};
        evt.type = OTOOL_LLM_TEXT_EVENT_RESPONSE_STARTED;
        evt.response_id = "chat_2";
        cb(&evt, user_ctx);
        evt.type = OTOOL_LLM_TEXT_EVENT_TEXT_DELTA;
        evt.data.text_delta.data = "状态正常";
        evt.data.text_delta.data_len = 12;
        cb(&evt, user_ctx);
        evt.type = OTOOL_LLM_TEXT_EVENT_COMPLETED;
        cb(&evt, user_ctx);
        return ESP_OK;
    }

    if (g_fake_turn == 1 || g_fake_loop) {
        /* turn 1: 模型要调用 get_device_status（loop 模式：每轮都调用同一工具） */
        otool_llm_text_event_t evt = {};
        evt.type = OTOOL_LLM_TEXT_EVENT_RESPONSE_STARTED;
        evt.response_id = "resp_1";
        cb(&evt, user_ctx);

        evt.type = OTOOL_LLM_TEXT_EVENT_TOOL_CALL_STARTED;
        evt.data.tool_call_started.output_index = 0;
        evt.data.tool_call_started.call_id = "call_1";
        evt.data.tool_call_started.name = g_fake_tool_name;
        cb(&evt, user_ctx);

        if (g_fake_interleaved) {
            evt.type = OTOOL_LLM_TEXT_EVENT_TOOL_CALL_STARTED;
            evt.data.tool_call_started.output_index = 1;
            evt.data.tool_call_started.call_id = "call_2";
            evt.data.tool_call_started.name = g_fake_tool_name;
            cb(&evt, user_ctx);

            evt.type = OTOOL_LLM_TEXT_EVENT_TOOL_ARGUMENTS_DELTA;
            evt.data.tool_arguments_delta.output_index = 0;
            evt.data.tool_arguments_delta.call_id = "call_1";
            evt.data.tool_arguments_delta.delta = "{}";
            evt.data.tool_arguments_delta.delta_len = 2;
            cb(&evt, user_ctx);

            evt.type = OTOOL_LLM_TEXT_EVENT_TOOL_ARGUMENTS_DELTA;
            evt.data.tool_arguments_delta.output_index = 1;
            evt.data.tool_arguments_delta.call_id = "call_2";
            evt.data.tool_arguments_delta.delta = "{\"x\":1}";
            evt.data.tool_arguments_delta.delta_len = 7;
            cb(&evt, user_ctx);

            evt.type = OTOOL_LLM_TEXT_EVENT_TOOL_CALL_DONE;
            evt.data.tool_call_done.output_index = 1;
            evt.data.tool_call_done.call_id = "call_2";
            evt.data.tool_call_done.name = g_fake_tool_name;
            evt.data.tool_call_done.arguments = "{\"x\":1}";
            evt.data.tool_call_done.arguments_len = 7;
            cb(&evt, user_ctx);

            evt.type = OTOOL_LLM_TEXT_EVENT_TOOL_CALL_DONE;
            evt.data.tool_call_done.output_index = 0;
            evt.data.tool_call_done.call_id = "call_1";
            evt.data.tool_call_done.name = g_fake_tool_name;
            evt.data.tool_call_done.arguments = "{}";
            evt.data.tool_call_done.arguments_len = 2;
            cb(&evt, user_ctx);

            evt.type = OTOOL_LLM_TEXT_EVENT_COMPLETED;
            cb(&evt, user_ctx);
            return ESP_OK;
        }

        evt.type = OTOOL_LLM_TEXT_EVENT_TOOL_ARGUMENTS_DELTA;
        evt.data.tool_arguments_delta.output_index = 0;
        evt.data.tool_arguments_delta.delta = "{}";
        evt.data.tool_arguments_delta.delta_len = 2;
        cb(&evt, user_ctx);

        evt.type = OTOOL_LLM_TEXT_EVENT_TOOL_CALL_DONE;
        evt.data.tool_call_done.output_index = 0;
        evt.data.tool_call_done.call_id = "call_1";
        evt.data.tool_call_done.name = g_fake_tool_name;
        evt.data.tool_call_done.arguments = "{}";
        evt.data.tool_call_done.arguments_len = 2;
        cb(&evt, user_ctx);

        evt.type = OTOOL_LLM_TEXT_EVENT_COMPLETED;
        cb(&evt, user_ctx);
        if (g_fake_multi) {
            /* 第二个工具调用（同工具、不同参数；output_index=1） */
            evt.type = OTOOL_LLM_TEXT_EVENT_RESPONSE_STARTED;
            evt.response_id = "resp_1b";
            cb(&evt, user_ctx);

            evt.type = OTOOL_LLM_TEXT_EVENT_TOOL_CALL_STARTED;
            evt.data.tool_call_started.output_index = 1;
            evt.data.tool_call_started.call_id = "call_2";
            evt.data.tool_call_started.name = g_fake_tool_name;
            cb(&evt, user_ctx);

            evt.type = OTOOL_LLM_TEXT_EVENT_TOOL_ARGUMENTS_DELTA;
            evt.data.tool_arguments_delta.output_index = 1;
            evt.data.tool_arguments_delta.delta = "{\"x\":1}";
            evt.data.tool_arguments_delta.delta_len = 7;
            cb(&evt, user_ctx);

            evt.type = OTOOL_LLM_TEXT_EVENT_TOOL_CALL_DONE;
            evt.data.tool_call_done.output_index = 1;
            evt.data.tool_call_done.call_id = "call_2";
            evt.data.tool_call_done.name = g_fake_tool_name;
            evt.data.tool_call_done.arguments = "{\"x\":1}";
            evt.data.tool_call_done.arguments_len = 7;
            cb(&evt, user_ctx);

            evt.type = OTOOL_LLM_TEXT_EVENT_COMPLETED;
            cb(&evt, user_ctx);
        }
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

static int g_device_exec_count = 0;
static int g_tool_output_mode = 0;

static esp_err_t tool_get_device_status(const char *arguments_json, char *output_json,
                                        size_t output_capacity, size_t *output_length,
                                        const otool_llm_tool_exec_context_t *exec_ctx,
                                        void *user_ctx)
{
    (void)arguments_json;
    (void)exec_ctx;
    (void)user_ctx;
    g_device_exec_count++;
    if (g_tool_output_mode == 1) {
        output_json[0] = '{';
        *output_length = output_capacity; /* callback contract violation: beyond byte budget */
        return ESP_OK;
    }
    if (g_tool_output_mode == 2) {
        memcpy(output_json, "not-json", 9);
        *output_length = 8;
        return ESP_OK;
    }
    if (g_tool_output_mode == 3) {
        output_json[0] = '{';
        output_json[1] = '}';
        output_json[2] = 'X'; /* claimed end is not NUL */
        *output_length = 2;
        return ESP_OK;
    }
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

static void agent_collect_event(agent_collector_t *col, const otool_llm_agent_event_t *evt)
{
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
}

static otool_llm_event_action_t agent_collect_cb(const otool_llm_agent_event_t *evt, void *user_ctx)
{
    agent_collector_t *col = (agent_collector_t *)user_ctx;
    agent_collect_event(col, evt);
    return OTOOL_LLM_EVENT_ACTION_CONTINUE;
}

/* 收到 TOOL_EXECUTION_STARTED 后请求取消（callback-driven cancel） */
static otool_llm_event_action_t agent_cancel_on_tool_start_cb(const otool_llm_agent_event_t *evt,
                                                              void *user_ctx)
{
    agent_collector_t *col = (agent_collector_t *)user_ctx;
    agent_collect_event(col, evt);
    if (evt->type == OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_STARTED) {
        return OTOOL_LLM_EVENT_ACTION_CANCEL;
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

/* ---- policy（副作用工具授权） ---- */

static int g_side_effect_exec_count = 0;

static esp_err_t tool_set_volume(const char *arguments_json, char *output_json,
                                 size_t output_capacity, size_t *output_length,
                                 const otool_llm_tool_exec_context_t *exec_ctx, void *user_ctx)
{
    (void)arguments_json;
    (void)exec_ctx;
    (void)user_ctx;
    g_side_effect_exec_count++;
    snprintf(output_json, output_capacity, "{\"ok\":true,\"volume\":5}");
    *output_length = strlen(output_json);
    return ESP_OK;
}

static otool_llm_tool_decision_t g_policy_decision = OTOOL_LLM_TOOL_DECISION_DENY;
static int g_policy_calls = 0;

static otool_llm_tool_decision_t test_policy(const char *tool_name, const char *arguments_json,
                                             uint32_t tool_flags, void *user_ctx)
{
    (void)tool_name;
    (void)arguments_json;
    (void)tool_flags;
    (void)user_ctx;
    g_policy_calls++;
    return g_policy_decision;
}

static void test_agent_policy(void)
{
    otool_llm_tool_registry_handle_t reg = NULL;
    CHECK(otool_llm_tool_registry_create(&reg) == ESP_OK, "policy reg create");
    otool_llm_tool_definition_t tool = {};
    tool.struct_size = sizeof(tool);
    tool.name = "set_volume";
    tool.description = "set volume";
    tool.parameters_json_schema =
        "{\"type\":\"object\",\"properties\":{},\"required\":[],\"additionalProperties\":false}";
    tool.flags = OTOOL_LLM_TOOL_SIDE_EFFECTING;
    tool.execute = tool_set_volume;
    CHECK(otool_llm_tool_registry_add(reg, &tool) == ESP_OK, "policy add tool");
    otool_llm_tool_registry_seal(reg);

    g_fake_tool_name = "set_volume";

    /* case 1: policy == NULL → 副作用工具必须被拒绝，executor 不得执行 */
    otool_llm_agent_config_t cfg = {};
    cfg.struct_size = sizeof(cfg);
    cfg.client = (otool_llm_client_handle_t)0x2;
    cfg.tools = reg;
    cfg.model = "fake-model";
    cfg.max_turns = 4;
    cfg.max_tool_calls = 4;
    cfg.policy = NULL;

    otool_llm_agent_handle_t agent = NULL;
    CHECK(otool_llm_agent_create(&cfg, &agent) == ESP_OK, "policy agent create (null policy)");

    agent_collector_t col = { 0 };
    g_fake_turn = 0;
    g_side_effect_exec_count = 0;
    esp_err_t err = otool_llm_agent_run_stream(agent, "调音量", agent_collect_cb, &col);
    CHECK(err == ESP_OK, "policy null run ok (0x%x)", (unsigned)err);
    bool saw_failed = false;
    for (int i = 0; i < col.count; i++) {
        if (col.events[i].type == OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_FAILED) {
            saw_failed = true;
            CHECK(strstr(col.events[i].data.tool_execution_finished.output, "permission_denied") !=
                  NULL,
                  "denied reason in output");
        }
    }
    CHECK(saw_failed, "policy NULL: tool execution failed");
    CHECK_EQ_T(g_side_effect_exec_count, 0);
    otool_llm_agent_destroy(agent);

    /* case 2: policy 返回 DENY → 拒绝且不执行 */
    cfg.policy = test_policy;
    g_policy_decision = OTOOL_LLM_TOOL_DECISION_DENY;
    g_policy_calls = 0;
    g_side_effect_exec_count = 0;
    CHECK(otool_llm_agent_create(&cfg, &agent) == ESP_OK, "policy agent create (deny)");
    col = (agent_collector_t){ 0 };
    g_fake_turn = 0;
    err = otool_llm_agent_run_stream(agent, "调音量", agent_collect_cb, &col);
    CHECK(err == ESP_OK, "policy deny run ok (0x%x)", (unsigned)err);
    saw_failed = false;
    for (int i = 0; i < col.count; i++) {
        if (col.events[i].type == OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_FAILED) {
            saw_failed = true;
        }
    }
    CHECK(saw_failed, "policy DENY: tool execution failed");
    CHECK(g_policy_calls >= 1, "policy callback invoked");
    CHECK_EQ_T(g_side_effect_exec_count, 0);
    otool_llm_agent_destroy(agent);

    /* case 3: policy 返回 ALLOW → 执行成功 */
    g_policy_decision = OTOOL_LLM_TOOL_DECISION_ALLOW;
    g_policy_calls = 0;
    g_side_effect_exec_count = 0;
    CHECK(otool_llm_agent_create(&cfg, &agent) == ESP_OK, "policy agent create (allow)");
    col = (agent_collector_t){ 0 };
    g_fake_turn = 0;
    err = otool_llm_agent_run_stream(agent, "调音量", agent_collect_cb, &col);
    CHECK(err == ESP_OK, "policy allow run ok (0x%x)", (unsigned)err);
    bool saw_finished = false;
    for (int i = 0; i < col.count; i++) {
        if (col.events[i].type == OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_FINISHED) {
            saw_finished = true;
            CHECK(strstr(col.events[i].data.tool_execution_finished.output, "\"ok\":true") != NULL,
                  "executor output ok");
        }
    }
    CHECK(saw_finished, "policy ALLOW: tool executed");
    CHECK(g_policy_calls >= 1, "policy callback invoked (allow)");
    CHECK_EQ_T(g_side_effect_exec_count, 1);
    otool_llm_agent_destroy(agent);

    otool_llm_tool_registry_destroy(reg);
    g_fake_tool_name = "get_device_status";
}

static void test_agent_loop_detection(void)
{
    /* 模型每轮请求同一工具同一参数 → 第二轮触发 RUN_LIMIT_REACHED，工具只执行一次 */
    otool_llm_tool_registry_handle_t reg = NULL;
    CHECK(otool_llm_tool_registry_create(&reg) == ESP_OK, "loop reg create");
    otool_llm_tool_definition_t tool = {};
    tool.struct_size = sizeof(tool);
    tool.name = "get_device_status";
    tool.description = "status";
    tool.parameters_json_schema =
        "{\"type\":\"object\",\"properties\":{},\"required\":[],\"additionalProperties\":false}";
    tool.flags = OTOOL_LLM_TOOL_READ_ONLY;
    tool.execute = tool_get_device_status;
    CHECK(otool_llm_tool_registry_add(reg, &tool) == ESP_OK, "loop add tool");
    otool_llm_tool_registry_seal(reg);

    otool_llm_agent_config_t cfg = {};
    cfg.struct_size = sizeof(cfg);
    cfg.client = (otool_llm_client_handle_t)0x2;
    cfg.tools = reg;
    cfg.model = "fake-model";
    cfg.max_turns = 6;
    cfg.max_tool_calls = 6;

    otool_llm_agent_handle_t agent = NULL;
    CHECK(otool_llm_agent_create(&cfg, &agent) == ESP_OK, "loop agent create");

    agent_collector_t col = { 0 };
    g_fake_turn = 0;
    g_fake_loop = 1;
    esp_err_t err = otool_llm_agent_run_stream(agent, "状态？", agent_collect_cb, &col);
    g_fake_loop = 0;
    CHECK(err == ESP_OK, "loop run ok (0x%x)", (unsigned)err);

    otool_llm_agent_event_type_t last = col.events[col.count - 1].type;
    CHECK(last == OTOOL_LLM_AGENT_EVENT_RUN_LIMIT_REACHED, "loop terminal RUN_LIMIT_REACHED (got %d)",
          (int)last);

    int exec_finished = 0;
    for (int i = 0; i < col.count; i++) {
        if (col.events[i].type == OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_FINISHED) {
            exec_finished++;
        }
    }
    CHECK_EQ_T(exec_finished, 1); /* 第二轮检测到循环，不重复执行 */

    otool_llm_agent_destroy(agent);
    otool_llm_tool_registry_destroy(reg);
}

static void test_agent_callback_cancel(void)
{
    /* 回调在工具执行开始后返回 CANCEL → run 以 CANCELLED 终止，无 RUN_COMPLETED */
    otool_llm_tool_registry_handle_t reg = NULL;
    CHECK(otool_llm_tool_registry_create(&reg) == ESP_OK, "cb-cancel reg create");
    otool_llm_tool_definition_t tool = {};
    tool.struct_size = sizeof(tool);
    tool.name = "get_device_status";
    tool.description = "status";
    tool.parameters_json_schema =
        "{\"type\":\"object\",\"properties\":{},\"required\":[],\"additionalProperties\":false}";
    tool.flags = OTOOL_LLM_TOOL_READ_ONLY;
    tool.execute = tool_get_device_status;
    CHECK(otool_llm_tool_registry_add(reg, &tool) == ESP_OK, "cb-cancel add tool");
    otool_llm_tool_registry_seal(reg);

    otool_llm_agent_config_t cfg = {};
    cfg.struct_size = sizeof(cfg);
    cfg.client = (otool_llm_client_handle_t)0x2;
    cfg.tools = reg;
    cfg.model = "fake-model";
    cfg.max_turns = 4;
    cfg.max_tool_calls = 4;

    otool_llm_agent_handle_t agent = NULL;
    CHECK(otool_llm_agent_create(&cfg, &agent) == ESP_OK, "cb-cancel agent create");

    agent_collector_t col = { 0 };
    g_fake_turn = 0;
    g_device_exec_count = 0;
    esp_err_t err = otool_llm_agent_run_stream(agent, "状态？", agent_cancel_on_tool_start_cb, &col);
    CHECK(err == ESP_OK, "cb-cancel run ok (0x%x)", (unsigned)err);

    otool_llm_agent_event_type_t last = col.events[col.count - 1].type;
    CHECK(last == OTOOL_LLM_AGENT_EVENT_CANCELLED, "cb-cancel terminal CANCELLED (got %d)",
          (int)last);
    for (int i = 0; i < col.count; i++) {
        CHECK(col.events[i].type != OTOOL_LLM_AGENT_EVENT_RUN_COMPLETED,
              "no RUN_COMPLETED after callback cancel");
    }
    CHECK_EQ_T(g_device_exec_count, 0); /* cancellation cannot cross into the executor */

    otool_llm_agent_destroy(agent);
    otool_llm_tool_registry_destroy(reg);
}

static esp_err_t tool_always_fail(const char *arguments_json, char *output_json,
                                  size_t output_capacity, size_t *output_length,
                                  const otool_llm_tool_exec_context_t *exec_ctx, void *user_ctx)
{
    (void)arguments_json;
    (void)exec_ctx;
    (void)user_ctx;
    snprintf(output_json, output_capacity, "{\"ok\":false,\"error\":{\"code\":\"boom\"}}");
    *output_length = strlen(output_json);
    return ESP_FAIL;
}

static esp_err_t tool_empty_result(const char *arguments_json, char *output_json,
                                   size_t output_capacity, size_t *output_length,
                                   const otool_llm_tool_exec_context_t *exec_ctx, void *user_ctx)
{
    (void)arguments_json;
    (void)exec_ctx;
    (void)user_ctx;
    snprintf(output_json, output_capacity, "{}");
    *output_length = strlen(output_json);
    return ESP_OK;
}

static void test_agent_tool_failures(void)
{
    /* unknown tool / 业务失败 / 空结果：都产生 FAILED 事件（或空 FINISHED），
     * run 不终止，模型在下一轮拿到结果继续回答。 */
    otool_llm_tool_registry_handle_t reg = NULL;
    CHECK(otool_llm_tool_registry_create(&reg) == ESP_OK, "fail reg create");
    otool_llm_tool_definition_t tool = {};
    tool.struct_size = sizeof(tool);
    tool.name = "get_device_status";
    tool.description = "status";
    tool.parameters_json_schema =
        "{\"type\":\"object\",\"properties\":{},\"required\":[],\"additionalProperties\":false}";
    tool.flags = OTOOL_LLM_TOOL_READ_ONLY;
    tool.execute = tool_get_device_status;
    CHECK(otool_llm_tool_registry_add(reg, &tool) == ESP_OK, "fail add ok tool");

    otool_llm_tool_definition_t ftool = {};
    ftool.struct_size = sizeof(ftool);
    ftool.name = "always_fail";
    ftool.description = "always fails";
    ftool.parameters_json_schema =
        "{\"type\":\"object\",\"properties\":{},\"required\":[],\"additionalProperties\":false}";
    ftool.flags = OTOOL_LLM_TOOL_READ_ONLY;
    ftool.execute = tool_always_fail;
    CHECK(otool_llm_tool_registry_add(reg, &ftool) == ESP_OK, "fail add fail tool");

    otool_llm_tool_definition_t etool = {};
    etool.struct_size = sizeof(etool);
    etool.name = "empty_tool";
    etool.description = "empty result";
    etool.parameters_json_schema =
        "{\"type\":\"object\",\"properties\":{},\"required\":[],\"additionalProperties\":false}";
    etool.flags = OTOOL_LLM_TOOL_READ_ONLY;
    etool.execute = tool_empty_result;
    CHECK(otool_llm_tool_registry_add(reg, &etool) == ESP_OK, "fail add empty tool");
    otool_llm_tool_registry_seal(reg);

    otool_llm_agent_config_t cfg = {};
    cfg.struct_size = sizeof(cfg);
    cfg.client = (otool_llm_client_handle_t)0x2;
    cfg.tools = reg;
    cfg.model = "fake-model";
    cfg.max_turns = 4;
    cfg.max_tool_calls = 4;

    /* case 1: unknown tool（未注册）→ FAILED(unknown_tool)，run 继续到 RUN_COMPLETED */
    otool_llm_agent_handle_t agent = NULL;
    CHECK(otool_llm_agent_create(&cfg, &agent) == ESP_OK, "fail agent create");
    agent_collector_t col = { 0 };
    g_fake_turn = 0;
    g_fake_tool_name = "no_such_tool";
    esp_err_t err = otool_llm_agent_run_stream(agent, "x", agent_collect_cb, &col);
    CHECK(err == ESP_OK, "unknown tool run ok (0x%x)", (unsigned)err);
    CHECK(col.events[col.count - 1].type == OTOOL_LLM_AGENT_EVENT_RUN_COMPLETED,
          "unknown tool still completes (got %d)", (int)col.events[col.count - 1].type);
    bool saw_unknown = false;
    for (int i = 0; i < col.count; i++) {
        if (col.events[i].type == OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_FAILED) {
            CHECK(strstr(col.events[i].data.tool_execution_finished.output, "unknown_tool") != NULL,
                  "unknown_tool reason");
            saw_unknown = true;
        }
    }
    CHECK(saw_unknown, "unknown tool FAILED event");
    otool_llm_agent_destroy(agent);

    /* case 2: 工具返回业务失败 → FAILED(tool_failed)，run 继续 */
    CHECK(otool_llm_agent_create(&cfg, &agent) == ESP_OK, "fail agent create 2");
    col = (agent_collector_t){ 0 };
    g_fake_turn = 0;
    g_fake_tool_name = "always_fail";
    err = otool_llm_agent_run_stream(agent, "x", agent_collect_cb, &col);
    CHECK(err == ESP_OK, "fail tool run ok (0x%x)", (unsigned)err);
    CHECK(col.events[col.count - 1].type == OTOOL_LLM_AGENT_EVENT_RUN_COMPLETED,
          "fail tool still completes (got %d)", (int)col.events[col.count - 1].type);
    bool saw_failed = false;
    for (int i = 0; i < col.count; i++) {
        if (col.events[i].type == OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_FAILED) {
            CHECK(strstr(col.events[i].data.tool_execution_finished.output, "tool_failed") != NULL,
                  "tool_failed reason");
            saw_failed = true;
        }
    }
    CHECK(saw_failed, "fail tool FAILED event");
    otool_llm_agent_destroy(agent);

    /* case 3: 空结果 → FINISHED（空 JSON），run 继续 */
    CHECK(otool_llm_agent_create(&cfg, &agent) == ESP_OK, "fail agent create 3");
    col = (agent_collector_t){ 0 };
    g_fake_turn = 0;
    g_fake_tool_name = "empty_tool";
    err = otool_llm_agent_run_stream(agent, "x", agent_collect_cb, &col);
    CHECK(err == ESP_OK, "empty tool run ok (0x%x)", (unsigned)err);
    CHECK(col.events[col.count - 1].type == OTOOL_LLM_AGENT_EVENT_RUN_COMPLETED,
          "empty tool still completes (got %d)", (int)col.events[col.count - 1].type);
    bool saw_finished = false;
    for (int i = 0; i < col.count; i++) {
        if (col.events[i].type == OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_FINISHED) {
            saw_finished = true;
        }
    }
    CHECK(saw_finished, "empty tool FINISHED event");
    otool_llm_agent_destroy(agent);

    otool_llm_tool_registry_destroy(reg);
    g_fake_tool_name = "get_device_status";
}

static void test_agent_multi_tools(void)
{
    /* 一轮两个工具调用（不同参数）→ 都执行，turn2 回传两个 tool_outputs */
    otool_llm_tool_registry_handle_t reg = NULL;
    CHECK(otool_llm_tool_registry_create(&reg) == ESP_OK, "multi reg create");
    otool_llm_tool_definition_t tool = {};
    tool.struct_size = sizeof(tool);
    tool.name = "get_device_status";
    tool.description = "status";
    tool.parameters_json_schema =
        "{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"integer\"}},"
        "\"required\":[],\"additionalProperties\":false}";
    tool.flags = OTOOL_LLM_TOOL_READ_ONLY;
    tool.execute = tool_get_device_status;
    CHECK(otool_llm_tool_registry_add(reg, &tool) == ESP_OK, "multi add tool");
    otool_llm_tool_registry_seal(reg);

    otool_llm_agent_config_t cfg = {};
    cfg.struct_size = sizeof(cfg);
    cfg.client = (otool_llm_client_handle_t)0x2;
    cfg.tools = reg;
    cfg.model = "fake-model";
    cfg.max_turns = 4;
    cfg.max_tool_calls = 4;

    otool_llm_agent_handle_t agent = NULL;
    CHECK(otool_llm_agent_create(&cfg, &agent) == ESP_OK, "multi agent create");

    agent_collector_t col = { 0 };
    g_fake_turn = 0;
    g_fake_multi = 1;
    g_last_request = NULL;
    esp_err_t err = otool_llm_agent_run_stream(agent, "状态？", agent_collect_cb, &col);
    g_fake_multi = 0;
    CHECK(err == ESP_OK, "multi run ok (0x%x)", (unsigned)err);
    CHECK(col.events[col.count - 1].type == OTOOL_LLM_AGENT_EVENT_RUN_COMPLETED,
          "multi RUN_COMPLETED (got %d)", (int)col.events[col.count - 1].type);

    int exec_finished = 0;
    for (int i = 0; i < col.count; i++) {
        if (col.events[i].type == OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_FINISHED) {
            exec_finished++;
        }
    }
    CHECK_EQ_T(exec_finished, 2);

    CHECK(g_last_request != NULL, "multi last request captured");
    if (g_last_request != NULL) {
        CHECK(g_last_request->tool_output_count == 2, "two tool outputs (got %u)",
              (unsigned)g_last_request->tool_output_count);
        if (g_last_request->tool_output_count == 2) {
            CHECK_STR(g_last_request->tool_outputs[0].call_id, "call_1");
            CHECK_STR(g_last_request->tool_outputs[1].call_id, "call_2");
        }
    }

    otool_llm_agent_destroy(agent);
    otool_llm_tool_registry_destroy(reg);
}

static void test_agent_context_full(void)
{
    /* Chat 模式多轮工具：transcript 达到上限 → CONTEXT_FULL（§10.2，不静默删除） */
    otool_llm_tool_registry_handle_t reg = NULL;
    CHECK(otool_llm_tool_registry_create(&reg) == ESP_OK, "cf reg create");
    otool_llm_tool_definition_t tool = {};
    tool.struct_size = sizeof(tool);
    tool.name = "get_device_status";
    tool.description = "status";
    tool.parameters_json_schema =
        "{\"type\":\"object\",\"properties\":{},\"required\":[],\"additionalProperties\":false}";
    tool.flags = OTOOL_LLM_TOOL_READ_ONLY;
    tool.execute = tool_get_device_status;
    CHECK(otool_llm_tool_registry_add(reg, &tool) == ESP_OK, "cf add tool");
    otool_llm_tool_registry_seal(reg);

    otool_llm_agent_config_t cfg = {};
    cfg.struct_size = sizeof(cfg);
    cfg.client = (otool_llm_client_handle_t)0x2;
    cfg.tools = reg;
    cfg.model = "fake-model";
    cfg.state_mode = OTOOL_LLM_AGENT_STATE_LOCAL_TRANSCRIPT;
    cfg.max_turns = 16; /* 1 user + 每轮 assistant/tool，超过默认 24-message 预算 */
    cfg.max_tool_calls = 32;

    otool_llm_agent_handle_t agent = NULL;
    CHECK(otool_llm_agent_create(&cfg, &agent) == ESP_OK, "cf agent create");

    agent_collector_t col = { 0 };
    g_fake_turn = 0;
    g_fake_chat = 1;
    g_fake_loop = 1;
    esp_err_t err = otool_llm_agent_run_stream(agent, "状态？", agent_collect_cb, &col);
    g_fake_chat = 0;
    g_fake_loop = 0;
    CHECK(err == OTOOL_LLM_ERR_CONTEXT_FULL,
          "context full run returns CONTEXT_FULL (got 0x%x)", (unsigned)err);

    bool saw_error = false;
    for (int i = 0; i < col.count; i++) {
        if (col.events[i].type == OTOOL_LLM_AGENT_EVENT_ERROR) {
            saw_error = true;
            CHECK(col.events[i].data.error.code == OTOOL_LLM_ERR_CONTEXT_FULL,
                  "ERROR event code CONTEXT_FULL (got 0x%x)",
                  (unsigned)col.events[i].data.error.code);
        }
        CHECK(col.events[i].type != OTOOL_LLM_AGENT_EVENT_RUN_COMPLETED,
              "no RUN_COMPLETED after CONTEXT_FULL");
    }
    CHECK(saw_error, "CONTEXT_FULL error event emitted");

    otool_llm_agent_destroy(agent);
    otool_llm_tool_registry_destroy(reg);
}

static void test_agent_chat_local_transcript(void)
{
    /* LOCAL_TRANSCRIPT + Chat 事件脚本：工具结果通过 transcript 消息回传 */
    otool_llm_tool_registry_handle_t reg = NULL;
    otool_llm_tool_registry_create(&reg);
    otool_llm_tool_definition_t tool = {};
    tool.struct_size = sizeof(tool);
    tool.name = "get_device_status";
    tool.description = "status";
    tool.parameters_json_schema =
        "{\"type\":\"object\",\"properties\":{},\"required\":[],\"additionalProperties\":false}";
    tool.flags = OTOOL_LLM_TOOL_READ_ONLY;
    tool.execute = tool_get_device_status;
    otool_llm_tool_registry_add(reg, &tool);
    otool_llm_tool_registry_seal(reg);

    otool_llm_agent_config_t cfg = {};
    cfg.struct_size = sizeof(cfg);
    cfg.client = (otool_llm_client_handle_t)0x2;
    cfg.tools = reg;
    cfg.model = "fake-model";
    cfg.state_mode = OTOOL_LLM_AGENT_STATE_LOCAL_TRANSCRIPT;
    cfg.max_turns = 4;
    cfg.max_tool_calls = 4;

    otool_llm_agent_handle_t agent = NULL;
    CHECK(otool_llm_agent_create(&cfg, &agent) == ESP_OK, "agent create (chat)");

    agent_collector_t col = { 0 };
    g_fake_turn = 0;
    g_last_request = NULL;
    g_fake_chat = 1;
    esp_err_t err = otool_llm_agent_run_stream(agent, "设备状态如何？", agent_collect_cb, &col);
    g_fake_chat = 0;
    CHECK(err == ESP_OK, "chat run ok (0x%x)", (unsigned)err);

    otool_llm_agent_event_type_t last = col.events[col.count - 1].type;
    CHECK(last == OTOOL_LLM_AGENT_EVENT_RUN_COMPLETED, "chat RUN_COMPLETED (got %d)", (int)last);
    CHECK_STR(col.text, "状态正常");

    /* transcript 链断言：第二轮请求 messages 含 assistant(tool_calls) + tool 消息 */
    CHECK(g_last_request != NULL, "chat last request captured");
    if (g_last_request != NULL) {
        CHECK(g_last_request->message_count >= 3, "chat messages >= 3 (got %u)",
              (unsigned)g_last_request->message_count);
        bool saw_assistant_calls = false;
        bool saw_tool_msg = false;
        for (size_t i = 0; i < g_last_request->message_count; i++) {
            const otool_llm_text_message_t *m = &g_last_request->messages[i];
            if (m->role == OTOOL_LLM_ROLE_ASSISTANT && m->tool_call_count > 0) {
                saw_assistant_calls = true;
                CHECK_STR(m->tool_calls[0].name, "get_device_status");
                CHECK_STR(m->tool_calls[0].id, "call_chat");
            }
            if (m->role == OTOOL_LLM_ROLE_TOOL) {
                saw_tool_msg = true;
                CHECK_STR(m->tool_call_id, "call_chat");
                CHECK(strstr(m->text, "\"uptime_s\"") != NULL, "tool result in transcript");
            }
        }
        CHECK(saw_assistant_calls, "assistant tool_calls message present");
        CHECK(saw_tool_msg, "tool role message present");
    }

    otool_llm_agent_destroy(agent);
    otool_llm_tool_registry_destroy(reg);
}

static otool_llm_tool_registry_handle_t make_status_registry(size_t max_output_bytes)
{
    otool_llm_tool_registry_handle_t reg = NULL;
    if (otool_llm_tool_registry_create(&reg) != ESP_OK) {
        return NULL;
    }
    otool_llm_tool_definition_t tool = {};
    tool.struct_size = sizeof(tool);
    tool.name = "get_device_status";
    tool.description = "status";
    tool.parameters_json_schema =
        "{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"integer\"}},"
        "\"required\":[],\"additionalProperties\":false}";
    tool.flags = OTOOL_LLM_TOOL_READ_ONLY;
    tool.max_output_bytes = max_output_bytes;
    tool.execute = tool_get_device_status;
    if (otool_llm_tool_registry_add(reg, &tool) != ESP_OK ||
        otool_llm_tool_registry_seal(reg) != ESP_OK) {
        otool_llm_tool_registry_destroy(reg);
        return NULL;
    }
    return reg;
}

static void test_agent_create_contract(void)
{
    otool_llm_tool_registry_handle_t reg = NULL;
    CHECK(otool_llm_tool_registry_create(&reg) == ESP_OK, "contract registry create");
    otool_llm_agent_config_t cfg = {};
    cfg.struct_size = sizeof(cfg);
    cfg.client = (otool_llm_client_handle_t)0x2;
    cfg.tools = reg;
    cfg.model = "fake-model";
    otool_llm_agent_handle_t agent = NULL;
    CHECK(otool_llm_agent_create(&cfg, &agent) == ESP_ERR_INVALID_STATE,
          "unsealed registry rejected");
    CHECK(otool_llm_tool_registry_seal(reg) == ESP_OK, "contract registry seal");
    cfg.parallel_tool_calls = true;
    CHECK(otool_llm_agent_create(&cfg, &agent) == OTOOL_LLM_ERR_UNSUPPORTED,
          "parallel tool calls explicitly unsupported");
    cfg.parallel_tool_calls = false;
    CHECK(otool_llm_agent_create(&cfg, &agent) == ESP_OK, "sealed sequential agent accepted");
    otool_llm_agent_destroy(agent);
    otool_llm_tool_registry_destroy(reg);
}

static void test_agent_tool_output_contract(void)
{
    otool_llm_tool_registry_handle_t reg = make_status_registry(256);
    CHECK(reg != NULL, "output registry");
    otool_llm_agent_config_t cfg = {};
    cfg.struct_size = sizeof(cfg);
    cfg.client = (otool_llm_client_handle_t)0x2;
    cfg.tools = reg;
    cfg.model = "fake-model";
    cfg.max_turns = 4;
    cfg.max_tool_calls = 4;

    const char *expected_codes[] = {
        "tool_output_too_large", "tool_failed", "tool_output_too_large"
    };
    for (int mode = 1; mode <= 3; mode++) {
        otool_llm_agent_handle_t agent = NULL;
        CHECK(otool_llm_agent_create(&cfg, &agent) == ESP_OK, "output agent create mode %d", mode);
        agent_collector_t col = { 0 };
        g_fake_turn = 0;
        g_tool_output_mode = mode;
        esp_err_t err = otool_llm_agent_run_stream(agent, "status", agent_collect_cb, &col);
        CHECK(err == ESP_OK, "output failure is fed back to model mode %d", mode);
        bool saw_failed = false;
        for (int i = 0; i < col.count; i++) {
            if (col.events[i].type == OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_FAILED) {
                saw_failed = true;
                CHECK(strstr(col.events[i].data.tool_execution_finished.output,
                             expected_codes[mode - 1]) != NULL,
                      "stable output error code mode %d", mode);
            }
        }
        CHECK(saw_failed, "invalid output rejected mode %d", mode);
        otool_llm_agent_destroy(agent);
    }
    g_tool_output_mode = 0;
    otool_llm_tool_registry_destroy(reg);
}

static void test_agent_interleaved_tool_events(void)
{
    otool_llm_tool_registry_handle_t reg = make_status_registry(0);
    CHECK(reg != NULL, "interleaved registry");
    otool_llm_agent_config_t cfg = {};
    cfg.struct_size = sizeof(cfg);
    cfg.client = (otool_llm_client_handle_t)0x2;
    cfg.tools = reg;
    cfg.model = "fake-model";
    cfg.max_turns = 4;
    cfg.max_tool_calls = 4;
    otool_llm_agent_handle_t agent = NULL;
    CHECK(otool_llm_agent_create(&cfg, &agent) == ESP_OK, "interleaved agent");
    agent_collector_t col = { 0 };
    g_fake_turn = 0;
    g_fake_interleaved = 1;
    g_device_exec_count = 0;
    esp_err_t err = otool_llm_agent_run_stream(agent, "status", agent_collect_cb, &col);
    g_fake_interleaved = 0;
    CHECK(err == ESP_OK, "interleaved run");
    CHECK_EQ_T(g_device_exec_count, 2);
    CHECK(g_last_request != NULL && g_last_request->tool_output_count == 2,
          "two interleaved outputs returned");
    if (g_last_request != NULL && g_last_request->tool_output_count == 2) {
        CHECK_STR(g_last_request->tool_outputs[0].call_id, "call_1");
        CHECK_STR(g_last_request->tool_outputs[1].call_id, "call_2");
    }
    otool_llm_agent_destroy(agent);
    otool_llm_tool_registry_destroy(reg);
}

static void test_agent_incomplete_turn(void)
{
    otool_llm_tool_registry_handle_t reg = make_status_registry(0);
    CHECK(reg != NULL, "incomplete registry");
    otool_llm_agent_config_t cfg = {};
    cfg.struct_size = sizeof(cfg);
    cfg.client = (otool_llm_client_handle_t)0x2;
    cfg.tools = reg;
    cfg.model = "fake-model";
    cfg.max_turns = 4;
    cfg.max_tool_calls = 4;
    otool_llm_agent_handle_t agent = NULL;
    CHECK(otool_llm_agent_create(&cfg, &agent) == ESP_OK, "incomplete agent");

    agent_collector_t col = { 0 };
    g_fake_turn = 0;
    g_fake_incomplete = 1;
    esp_err_t err = otool_llm_agent_run_stream(agent, "status", agent_collect_cb, &col);
    g_fake_incomplete = 0;
    CHECK(err == ESP_OK, "incomplete run returns terminal success");
    CHECK(col.count > 0 &&
              col.events[col.count - 1].type == OTOOL_LLM_AGENT_EVENT_RUN_LIMIT_REACHED,
          "incomplete model turn cannot become RUN_COMPLETED");
    for (int i = 0; i < col.count; i++) {
        CHECK(col.events[i].type != OTOOL_LLM_AGENT_EVENT_RUN_COMPLETED,
              "no RUN_COMPLETED after incomplete turn");
    }

    otool_llm_agent_destroy(agent);
    otool_llm_tool_registry_destroy(reg);
}

static void test_agent_cross_run_sessions(void)
{
    otool_llm_tool_registry_handle_t reg = make_status_registry(0);
    CHECK(reg != NULL, "session registry");
    otool_llm_agent_config_t cfg = {};
    cfg.struct_size = sizeof(cfg);
    cfg.client = (otool_llm_client_handle_t)0x2;
    cfg.tools = reg;
    cfg.model = "fake-model";
    cfg.max_turns = 4;
    cfg.max_tool_calls = 4;

    /* Responses: the first request of run 2 must continue from run 1's final response id. */
    otool_llm_agent_handle_t agent = NULL;
    CHECK(otool_llm_agent_create(&cfg, &agent) == ESP_OK, "responses session agent");
    size_t obs_start = g_observation_count;
    g_fake_turn = 0;
    agent_collector_t col = { 0 };
    CHECK(otool_llm_agent_run_stream(agent, "第一问", agent_collect_cb, &col) == ESP_OK,
          "responses run 1");
    col = (agent_collector_t){ 0 };
    CHECK(otool_llm_agent_run_stream(agent, "第二问", agent_collect_cb, &col) == ESP_OK,
          "responses run 2");
    CHECK(g_observation_count >= obs_start + 3, "responses observations");
    if (g_observation_count >= obs_start + 3) {
        request_observation_t *second_run = &g_observations[obs_start + 2];
        CHECK(second_run->has_previous_response_id, "run 2 carries previous response id");
        CHECK_STR(second_run->previous_response_id, "resp_2");
        CHECK(second_run->message_count == 1, "run 2 has one new input");
        CHECK_STR(second_run->texts[0], "第二问");
    }
    otool_llm_agent_reset_session(agent);
    size_t reset_obs = g_observation_count;
    col = (agent_collector_t){ 0 };
    CHECK(otool_llm_agent_run_stream(agent, "重置后", agent_collect_cb, &col) == ESP_OK,
          "responses run after reset");
    CHECK(g_observation_count > reset_obs &&
              !g_observations[reset_obs].has_previous_response_id,
          "reset clears remote response chain");
    otool_llm_agent_destroy(agent);

    /* Chat: run 1 starts with exactly one user message; run 2 includes completed history once. */
    cfg.state_mode = OTOOL_LLM_AGENT_STATE_LOCAL_TRANSCRIPT;
    CHECK(otool_llm_agent_create(&cfg, &agent) == ESP_OK, "chat session agent");
    obs_start = g_observation_count;
    g_fake_turn = 0;
    g_fake_chat = 1;
    col = (agent_collector_t){ 0 };
    CHECK(otool_llm_agent_run_stream(agent, "聊天第一问", agent_collect_cb, &col) == ESP_OK,
          "chat run 1");
    col = (agent_collector_t){ 0 };
    CHECK(otool_llm_agent_run_stream(agent, "聊天第二问", agent_collect_cb, &col) == ESP_OK,
          "chat run 2");
    g_fake_chat = 0;
    CHECK(g_observation_count >= obs_start + 3, "chat observations");
    if (g_observation_count >= obs_start + 3) {
        request_observation_t *first = &g_observations[obs_start];
        request_observation_t *second_run = &g_observations[obs_start + 2];
        CHECK(first->message_count == 1, "first Chat request does not duplicate user");
        CHECK_STR(first->texts[0], "聊天第一问");
        CHECK(second_run->message_count == 5, "second Chat run has four history messages + user");
        int second_user_count = 0;
        for (size_t i = 0; i < second_run->message_count; i++) {
            if (second_run->roles[i] == OTOOL_LLM_ROLE_USER &&
                strcmp(second_run->texts[i], "聊天第二问") == 0) {
                second_user_count++;
            }
        }
        CHECK_EQ_T(second_user_count, 1);
    }
    otool_llm_agent_destroy(agent);
    otool_llm_tool_registry_destroy(reg);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_agent_tool_loop();
    test_agent_policy();
    test_agent_loop_detection();
    test_agent_callback_cancel();
    test_agent_tool_failures();
    test_agent_multi_tools();
    test_agent_context_full();
    test_agent_chat_local_transcript();
    test_agent_create_contract();
    test_agent_tool_output_contract();
    test_agent_interleaved_tool_events();
    test_agent_incomplete_turn();
    test_agent_cross_run_sessions();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
