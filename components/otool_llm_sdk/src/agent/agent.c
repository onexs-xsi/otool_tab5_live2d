/*
 * SPDX-FileCopyrightText: 2026 otool project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Basic Agent Runtime (WP4): bounded multi-turn loop over the text SDK with
 * Responses remote response chain (store=true + previous_response_id +
 * function_call_output). Platform-neutral core: only depends on the public
 * SDK APIs and esp_timer_get_time().
 */

#include "otool_llm_agent.h"

#include "esp_log.h"
#include "esp_timer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "llm_agent";

#ifndef CONFIG_OTOOL_LLM_MAX_AGENT_TURNS
#define CONFIG_OTOOL_LLM_MAX_AGENT_TURNS 6
#endif
#ifndef CONFIG_OTOOL_LLM_MAX_AGENT_TOOL_CALLS
#define CONFIG_OTOOL_LLM_MAX_AGENT_TOOL_CALLS 8
#endif
#ifndef CONFIG_OTOOL_LLM_DEFAULT_AGENT_TIMEOUT_MS
#define CONFIG_OTOOL_LLM_DEFAULT_AGENT_TIMEOUT_MS 120000
#endif
#ifndef CONFIG_OTOOL_LLM_DEFAULT_TOOL_TIMEOUT_MS
#define CONFIG_OTOOL_LLM_DEFAULT_TOOL_TIMEOUT_MS 15000
#endif
#ifndef CONFIG_OTOOL_LLM_MAX_TOOL_ARGUMENT_BYTES
#define CONFIG_OTOOL_LLM_MAX_TOOL_ARGUMENT_BYTES 4096
#endif
#ifndef CONFIG_OTOOL_LLM_MAX_TOOL_OUTPUT_BYTES
#define CONFIG_OTOOL_LLM_MAX_TOOL_OUTPUT_BYTES 4096
#endif
#ifndef CONFIG_OTOOL_LLM_MAX_PENDING_TOOL_CALLS
#define CONFIG_OTOOL_LLM_MAX_PENDING_TOOL_CALLS 2
#endif
#ifndef CONFIG_OTOOL_LLM_MAX_TOOLS
#define CONFIG_OTOOL_LLM_MAX_TOOLS 8
#endif

/* 单轮最多收集的工具调用（本地防御上限） */
#define AGENT_MAX_CALLS_PER_TURN CONFIG_OTOOL_LLM_MAX_PENDING_TOOL_CALLS

typedef struct {
    bool used;
    bool ready;
    char call_id[64];
    char name[64];
    char arguments[CONFIG_OTOOL_LLM_MAX_TOOL_ARGUMENT_BYTES];
    size_t arguments_len;
} agent_tool_call_t;

struct otool_llm_agent {
    otool_llm_client_handle_t client;
    otool_llm_tool_registry_handle_t tools;
    char *model;
    char *instructions;
    otool_llm_agent_state_mode_t state_mode;
    uint32_t max_turns;
    uint32_t max_tool_calls;
    uint32_t run_timeout_ms;
    bool parallel_tool_calls;
    otool_llm_tool_policy_cb_t policy;
    void *policy_ctx;
    /* run state */
    volatile bool running;
    volatile bool cancel_requested;
    otool_llm_request_handle_t active_request; /* 当前模型请求（cancel 用） */
    otool_llm_agent_event_cb_t callback;
    void *user_ctx;
    uint32_t turn_index;
    uint32_t tool_calls_done;
    /* 链状态 */
    char response_id[128];
    otool_llm_tool_output_t last_tool_outputs[AGENT_MAX_CALLS_PER_TURN];
    size_t last_output_count;
    /* 工具输出缓冲（跨 execute 存活，指向 last_tool_outputs） */
    char tool_output_bufs[AGENT_MAX_CALLS_PER_TURN][CONFIG_OTOOL_LLM_MAX_TOOL_OUTPUT_BYTES];
    char tool_call_ids[AGENT_MAX_CALLS_PER_TURN][64];
};

/* ---------------- 事件发射 ---------------- */

static void agent_emit(otool_llm_agent_handle_t agent, otool_llm_agent_event_t *evt)
{
    if (agent->callback != NULL) {
        otool_llm_event_action_t action = agent->callback(evt, agent->user_ctx);
        if (action == OTOOL_LLM_EVENT_ACTION_CANCEL) {
            agent->cancel_requested = true;
        }
    }
}

/* ---------------- 工具结果 JSON 生成 ---------------- */

static void tool_error_output(const char *code, const char *message, bool retryable,
                              char *out, size_t cap)
{
    snprintf(out, cap, "{\"ok\":false,\"error\":{\"code\":\"%s\",\"message\":\"%.200s\","
                       "\"retryable\":%s}}",
             code, message ? message : "", retryable ? "true" : "false");
}

/* ---------------- 模型轮次事件桥接 ---------------- */

typedef struct {
    otool_llm_agent_handle_t agent;
    agent_tool_call_t calls[AGENT_MAX_CALLS_PER_TURN];
    int call_count;
    char response_id[128];
    bool turn_ok;
    esp_err_t turn_error;
    otool_llm_usage_t usage;
    bool usage_set;
} bridge_ctx_t;

static otool_llm_event_action_t bridge_cb(const otool_llm_text_event_t *evt, void *user_ctx)
{
    bridge_ctx_t *b = (bridge_ctx_t *)user_ctx;
    otool_llm_agent_handle_t agent = b->agent;

    switch (evt->type) {
    case OTOOL_LLM_TEXT_EVENT_RESPONSE_STARTED:
        if (evt->response_id != NULL) {
            snprintf(b->response_id, sizeof(b->response_id), "%s", evt->response_id);
        }
        break;
    case OTOOL_LLM_TEXT_EVENT_TEXT_DELTA: {
        otool_llm_agent_event_t ae = { .type = OTOOL_LLM_AGENT_EVENT_TEXT_DELTA };
        ae.turn_index = agent->running ? agent->turn_index : 0;
        ae.data.text_delta.data = evt->data.text_delta.data;
        ae.data.text_delta.data_len = evt->data.text_delta.data_len;
        agent_emit(agent, &ae);
        break;
    }
    case OTOOL_LLM_TEXT_EVENT_TOOL_CALL_STARTED: {
        if (b->call_count < AGENT_MAX_CALLS_PER_TURN) {
            agent_tool_call_t *c = &b->calls[b->call_count];
            memset(c, 0, sizeof(*c));
            c->used = true;
            if (evt->data.tool_call_started.call_id != NULL) {
                snprintf(c->call_id, sizeof(c->call_id), "%s",
                         evt->data.tool_call_started.call_id);
            }
            if (evt->data.tool_call_started.name != NULL) {
                snprintf(c->name, sizeof(c->name), "%s", evt->data.tool_call_started.name);
            }
            otool_llm_agent_event_t ae = { .type = OTOOL_LLM_AGENT_EVENT_TOOL_CALL_STARTED };
            ae.turn_index = agent->turn_index;
            ae.tool_index = (uint32_t)b->call_count;
            ae.call_id = c->call_id[0] ? c->call_id : NULL;
            ae.name = c->name[0] ? c->name : NULL;
            agent_emit(agent, &ae);
            b->call_count++;
        }
        break;
    }
    case OTOOL_LLM_TEXT_EVENT_TOOL_ARGUMENTS_DELTA: {
        if (b->call_count > 0) {
            agent_tool_call_t *c = &b->calls[b->call_count - 1];
            size_t dlen = evt->data.tool_arguments_delta.delta_len;
            if (c->arguments_len + dlen < sizeof(c->arguments)) {
                memcpy(c->arguments + c->arguments_len, evt->data.tool_arguments_delta.delta, dlen);
                c->arguments_len += dlen;
                c->arguments[c->arguments_len] = '\0';
            }
            otool_llm_agent_event_t ae = { .type = OTOOL_LLM_AGENT_EVENT_TOOL_ARGUMENTS_DELTA };
            ae.turn_index = agent->turn_index;
            ae.tool_index = (uint32_t)(b->call_count - 1);
            ae.call_id = c->call_id[0] ? c->call_id : NULL;
            ae.data.tool_arguments_delta.delta = evt->data.tool_arguments_delta.delta;
            ae.data.tool_arguments_delta.delta_len = dlen;
            agent_emit(agent, &ae);
        }
        break;
    }
    case OTOOL_LLM_TEXT_EVENT_TOOL_CALL_DONE: {
        if (b->call_count > 0) {
            agent_tool_call_t *c = &b->calls[b->call_count - 1];
            /* 用事件中的完整 arguments 覆盖（SDK 已校验与 delta 一致） */
            if (evt->data.tool_call_done.arguments != NULL &&
                evt->data.tool_call_done.arguments_len < sizeof(c->arguments)) {
                memcpy(c->arguments, evt->data.tool_call_done.arguments,
                       evt->data.tool_call_done.arguments_len);
                c->arguments_len = evt->data.tool_call_done.arguments_len;
                c->arguments[c->arguments_len] = '\0';
            }
            c->ready = true;
            otool_llm_agent_event_t ae = { .type = OTOOL_LLM_AGENT_EVENT_TOOL_CALL_READY };
            ae.turn_index = agent->turn_index;
            ae.tool_index = (uint32_t)(b->call_count - 1);
            ae.call_id = c->call_id[0] ? c->call_id : NULL;
            ae.name = c->name[0] ? c->name : NULL;
            ae.data.tool_call_ready.arguments = c->arguments;
            ae.data.tool_call_ready.arguments_len = c->arguments_len;
            agent_emit(agent, &ae);
        }
        break;
    }
    case OTOOL_LLM_TEXT_EVENT_USAGE:
        b->usage = evt->data.usage;
        b->usage_set = true;
        break;
    case OTOOL_LLM_TEXT_EVENT_COMPLETED:
    case OTOOL_LLM_TEXT_EVENT_INCOMPLETE:
        b->turn_ok = true;
        if (b->usage_set) {
            otool_llm_agent_event_t ae = { .type = OTOOL_LLM_AGENT_EVENT_USAGE };
            ae.turn_index = agent->turn_index;
            ae.data.usage = b->usage;
            agent_emit(agent, &ae);
        }
        break;
    case OTOOL_LLM_TEXT_EVENT_ERROR:
        b->turn_error = evt->data.error.code;
        break;
    case OTOOL_LLM_TEXT_EVENT_CANCELLED:
        agent->cancel_requested = true;
        break;
    default:
        break;
    }
    return agent->cancel_requested ? OTOOL_LLM_EVENT_ACTION_CANCEL : OTOOL_LLM_EVENT_ACTION_CONTINUE;
}

/* ---------------- 工具执行 ---------------- */

static esp_err_t agent_execute_tool(otool_llm_agent_handle_t agent,
                                    const otool_llm_tool_definition_t *tool,
                                    const char *arguments, size_t arguments_len,
                                    char *output, size_t output_cap, size_t *output_len)
{
    if (tool->execute == NULL) {
        tool_error_output("tool_failed", "tool has no executor", false, output, output_cap);
        *output_len = strlen(output);
        return OTOOL_LLM_ERR_TOOL_FAILED;
    }

    uint32_t timeout = tool->timeout_ms > 0 ? tool->timeout_ms : CONFIG_OTOOL_LLM_DEFAULT_TOOL_TIMEOUT_MS;
    otool_llm_tool_exec_context_t exec_ctx = {
        .cancel_requested = &agent->cancel_requested,
        .deadline_us = esp_timer_get_time() + (int64_t)timeout * 1000,
    };
    esp_err_t err = tool->execute(arguments, output, output_cap, output_len, &exec_ctx,
                                  tool->user_ctx);
    if (err != ESP_OK) {
        if (agent->cancel_requested) {
            tool_error_output("cancelled", "cancelled", false, output, output_cap);
        } else {
            tool_error_output("tool_failed", "tool execution failed", true, output, output_cap);
        }
        *output_len = strlen(output);
        return err;
    }
    return ESP_OK;
}

/* ---------------- run ---------------- */

esp_err_t otool_llm_agent_run_stream(otool_llm_agent_handle_t agent,
                                     const char *user_text,
                                     otool_llm_agent_event_cb_t callback,
                                     void *user_ctx)
{
    if (agent == NULL || user_text == NULL || callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (agent->running) {
        return ESP_ERR_INVALID_STATE;
    }
    agent->running = true;
    agent->cancel_requested = false;
    agent->callback = callback;
    agent->user_ctx = user_ctx;
    agent->turn_index = 0;
    agent->last_output_count = 0;

    otool_llm_agent_event_t evt = { .type = OTOOL_LLM_AGENT_EVENT_RUN_STARTED };
    agent_emit(agent, &evt);

    int64_t deadline = esp_timer_get_time() + (int64_t)agent->run_timeout_ms * 1000;
    esp_err_t result = ESP_OK;
    bool finished = false;
    bool first_turn = true;
    uint32_t total_tool_calls = 0;

    while (!finished) {
        if (agent->cancel_requested) {
            evt.type = OTOOL_LLM_AGENT_EVENT_CANCELLED;
            agent_emit(agent, &evt);
            result = ESP_OK;
            break;
        }
        if (agent->turn_index >= agent->max_turns) {
            evt.type = OTOOL_LLM_AGENT_EVENT_RUN_LIMIT_REACHED;
            agent_emit(agent, &evt);
            result = ESP_OK;
            break;
        }
        if (esp_timer_get_time() > deadline) {
            evt.type = OTOOL_LLM_AGENT_EVENT_RUN_LIMIT_REACHED;
            agent_emit(agent, &evt);
            result = ESP_OK;
            break;
        }

        agent->turn_index++;
        evt.type = OTOOL_LLM_AGENT_EVENT_TURN_STARTED;
        evt.turn_index = agent->turn_index;
        agent_emit(agent, &evt);

        /* 收集 registry 工具（每轮重建工具数组） */
        otool_llm_tool_definition_t tools[CONFIG_OTOOL_LLM_MAX_TOOLS];
        size_t tool_count = 0;
        size_t reg_count = otool_llm_tool_registry_count(agent->tools);
        if (reg_count > CONFIG_OTOOL_LLM_MAX_TOOLS) {
            reg_count = CONFIG_OTOOL_LLM_MAX_TOOLS;
        }
        for (size_t i = 0; i < reg_count; i++) {
            const otool_llm_tool_definition_t *t = otool_llm_tool_registry_at(agent->tools, i);
            tools[i] = *t; /* 浅拷贝：registry 持有字符串 */
            tool_count++;
        }

        /* 构建请求 */
        otool_llm_text_message_t msg = { .role = OTOOL_LLM_ROLE_USER, .text = user_text };
        otool_llm_text_request_t req = {};
        req.struct_size = sizeof(req);
        req.model = agent->model;
        req.instructions = agent->instructions;
        req.max_output_tokens = 2048;
        req.store = true; /* REMOTE_RESPONSE_CHAIN */
        if (first_turn) {
            req.messages = &msg;
            req.message_count = 1;
        } else {
            req.tool_outputs = agent->last_tool_outputs;
            req.tool_output_count = agent->last_output_count;
            req.previous_response_id = agent->response_id[0] != '\0' ? agent->response_id : NULL;
        }
        req.tools = tool_count > 0 ? tools : NULL;
        req.tool_count = tool_count;

        otool_llm_request_handle_t request = NULL;
        esp_err_t err = otool_llm_request_create(agent->client, &req, &request);
        if (err != ESP_OK) {
            evt.type = OTOOL_LLM_AGENT_EVENT_ERROR;
            evt.data.error.code = err;
            evt.data.error.message = esp_err_to_name(err);
            agent_emit(agent, &evt);
            result = err;
            break;
        }
        agent->active_request = request;

        bridge_ctx_t bridge = { 0 };
        bridge.agent = agent;
        err = otool_llm_request_execute_stream(request, bridge_cb, &bridge);
        agent->active_request = NULL;
        otool_llm_request_destroy(request);

        if (agent->cancel_requested) {
            evt.type = OTOOL_LLM_AGENT_EVENT_CANCELLED;
            agent_emit(agent, &evt);
            result = ESP_OK;
            break;
        }
        if (err != ESP_OK && !bridge.turn_ok) {
            evt.type = OTOOL_LLM_AGENT_EVENT_ERROR;
            evt.data.error.code = err != ESP_OK ? err : OTOOL_LLM_ERR_PROTOCOL;
            evt.data.error.message = esp_err_to_name(err);
            agent_emit(agent, &evt);
            result = err;
            break;
        }
        if (bridge.response_id[0] != '\0') {
            snprintf(agent->response_id, sizeof(agent->response_id), "%s", bridge.response_id);
        }

        /* 本轮收集的工具调用 */
        int ready_count = 0;
        for (int i = 0; i < bridge.call_count; i++) {
            if (bridge.calls[i].used && bridge.calls[i].ready) {
                ready_count++;
            }
        }

        evt.type = OTOOL_LLM_AGENT_EVENT_TURN_COMPLETED;
        evt.turn_index = agent->turn_index;
        agent_emit(agent, &evt);

        if (ready_count == 0) {
            evt.type = OTOOL_LLM_AGENT_EVENT_RUN_COMPLETED;
            agent_emit(agent, &evt);
            result = ESP_OK;
            break;
        }

        /* 执行工具并收集输出 */
        size_t out_count = 0;
        for (int i = 0; i < bridge.call_count; i++) {
            agent_tool_call_t *c = &bridge.calls[i];
            if (!c->used || !c->ready) {
                continue;
            }
            if (total_tool_calls >= agent->max_tool_calls) {
                evt.type = OTOOL_LLM_AGENT_EVENT_RUN_LIMIT_REACHED;
                agent_emit(agent, &evt);
                finished = true;
                break;
            }
            total_tool_calls++;
            agent->tool_calls_done = total_tool_calls;

            evt.type = OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_STARTED;
            evt.turn_index = agent->turn_index;
            evt.tool_index = (uint32_t)i;
            evt.call_id = c->call_id[0] ? c->call_id : NULL;
            evt.name = c->name[0] ? c->name : NULL;
            agent_emit(agent, &evt);

            char output[CONFIG_OTOOL_LLM_MAX_TOOL_OUTPUT_BYTES];
            size_t output_len = 0;
            const otool_llm_tool_definition_t *tool =
                otool_llm_tool_registry_find(agent->tools, c->name);

            if (tool == NULL) {
                tool_error_output("unknown_tool", "tool is not registered", false,
                                  output, sizeof(output));
                output_len = strlen(output);
                evt.type = OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_FAILED;
            } else {
                /* policy：副作用工具默认拒绝，除非 policy 明确 ALLOW */
                bool allowed = true;
                if (tool->flags & (OTOOL_LLM_TOOL_SIDE_EFFECTING | OTOOL_LLM_TOOL_NEEDS_APPROVAL)) {
                    if (agent->policy == NULL) {
                        allowed = false;
                    } else {
                        allowed = agent->policy(c->name, c->arguments, tool->flags,
                                                agent->policy_ctx) == OTOOL_LLM_TOOL_DECISION_ALLOW;
                    }
                }
                if (!allowed) {
                    tool_error_output("permission_denied", "tool not approved", false,
                                      output, sizeof(output));
                    output_len = strlen(output);
                    evt.type = OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_FAILED;
                } else {
                    /* 本地参数校验（不可信输入） */
                    esp_err_t verr = otool_llm_tool_arguments_validate(tool, c->arguments,
                                                                       c->arguments_len);
                    if (verr != ESP_OK) {
                        tool_error_output("invalid_arguments", "arguments failed schema validation",
                                          false, output, sizeof(output));
                        output_len = strlen(output);
                        evt.type = OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_FAILED;
                    } else {
                        esp_err_t terr = agent_execute_tool(agent, tool, c->arguments,
                                                            c->arguments_len, output,
                                                            sizeof(output), &output_len);
                        evt.type = terr == ESP_OK ? OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_FINISHED
                                                  : OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_FAILED;
                    }
                }
            }
            evt.turn_index = agent->turn_index;
            evt.tool_index = (uint32_t)i;
            evt.call_id = c->call_id[0] ? c->call_id : NULL;
            evt.name = c->name[0] ? c->name : NULL;
            evt.data.tool_execution_finished.output = output;
            evt.data.tool_execution_finished.output_len = output_len;
            agent_emit(agent, &evt);

            /* 输出入队（本轮之后回传模型）：写入 agent 内部缓冲 */
            if (out_count < AGENT_MAX_CALLS_PER_TURN) {
                otool_llm_tool_output_t *out_item = &agent->last_tool_outputs[out_count];
                snprintf(agent->tool_call_ids[out_count], sizeof(agent->tool_call_ids[out_count]),
                         "%s", c->call_id[0] ? c->call_id : "call_unknown");
                snprintf(agent->tool_output_bufs[out_count],
                         sizeof(agent->tool_output_bufs[out_count]), "%s", output);
                out_item->call_id = agent->tool_call_ids[out_count];
                out_item->output = agent->tool_output_bufs[out_count];
                out_count++;
            }
            if (agent->cancel_requested) {
                break;
            }
        }
        agent->last_output_count = out_count;
        first_turn = false;
    }

    agent->running = false;
    return result;
}

esp_err_t otool_llm_agent_cancel(otool_llm_agent_handle_t agent)
{
    if (agent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    agent->cancel_requested = true;
    if (agent->active_request != NULL) {
        otool_llm_request_cancel(agent->active_request);
    }
    return ESP_OK;
}

void otool_llm_agent_reset_session(otool_llm_agent_handle_t agent)
{
    if (agent == NULL) {
        return;
    }
    agent->response_id[0] = '\0';
    agent->last_output_count = 0;
}

void otool_llm_agent_destroy(otool_llm_agent_handle_t agent)
{
    if (agent == NULL) {
        return;
    }
    if (agent->running) {
        ESP_LOGE(TAG, "refusing to destroy agent while a run is active");
        return;
    }
    free(agent->instructions);
    free(agent->model);
    free(agent);
}

esp_err_t otool_llm_agent_create(const otool_llm_agent_config_t *config,
                                 otool_llm_agent_handle_t *out_agent)
{
    if (config == NULL || out_agent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->struct_size < sizeof(otool_llm_agent_config_t)) {
        return ESP_ERR_INVALID_VERSION;
    }
    if (config->client == NULL || config->tools == NULL || config->model == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    otool_llm_agent_handle_t agent = (otool_llm_agent_handle_t)calloc(1, sizeof(*agent));
    if (agent == NULL) {
        return ESP_ERR_NO_MEM;
    }
    agent->client = config->client;
    agent->tools = config->tools;
    agent->state_mode = config->state_mode;
    agent->max_turns = config->max_turns > 0 ? config->max_turns : CONFIG_OTOOL_LLM_MAX_AGENT_TURNS;
    agent->max_tool_calls = config->max_tool_calls > 0 ? config->max_tool_calls
                                                       : CONFIG_OTOOL_LLM_MAX_AGENT_TOOL_CALLS;
    agent->run_timeout_ms = config->run_timeout_ms > 0 ? config->run_timeout_ms
                                                       : CONFIG_OTOOL_LLM_DEFAULT_AGENT_TIMEOUT_MS;
    agent->parallel_tool_calls = config->parallel_tool_calls;
    agent->policy = config->policy;
    agent->policy_ctx = config->policy_ctx;

    char *model = strdup(config->model);
    char *instructions = config->instructions != NULL ? strdup(config->instructions) : NULL;
    if (model == NULL || (config->instructions != NULL && instructions == NULL)) {
        free(model);
        free(instructions);
        free(agent);
        return ESP_ERR_NO_MEM;
    }
    agent->model = model;
    agent->instructions = instructions;

    *out_agent = agent;
    return ESP_OK;
}
