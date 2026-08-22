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

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

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
#ifndef CONFIG_OTOOL_LLM_MAX_TOOL_NAME_BYTES
#define CONFIG_OTOOL_LLM_MAX_TOOL_NAME_BYTES 64
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
#ifndef CONFIG_OTOOL_LLM_MAX_AGENT_MESSAGES
#define CONFIG_OTOOL_LLM_MAX_AGENT_MESSAGES 24
#endif
#ifndef CONFIG_OTOOL_LLM_MAX_AGENT_CONTEXT_BYTES
#define CONFIG_OTOOL_LLM_MAX_AGENT_CONTEXT_BYTES 32768
#endif

/* 单轮最多收集的工具调用（本地防御上限） */
#define AGENT_MAX_CALLS_PER_TURN CONFIG_OTOOL_LLM_MAX_PENDING_TOOL_CALLS

#define AGENT_TRANSCRIPT_MAX CONFIG_OTOOL_LLM_MAX_AGENT_MESSAGES

typedef struct {
    bool used;
    bool ready;
    uint32_t output_index;
    char call_id[64];
    char name[CONFIG_OTOOL_LLM_MAX_TOOL_NAME_BYTES + 1];
    char arguments[CONFIG_OTOOL_LLM_MAX_TOOL_ARGUMENT_BYTES + 1];
    size_t arguments_len;
} agent_tool_call_t;

/* 本地 transcript 条目（Chat 模式） */
typedef struct {
    otool_llm_role_t role;
    char *text;
    otool_llm_tool_call_msg_t *tool_calls;
    size_t tool_call_count;
    char *tool_call_id;
    size_t owned_bytes;
} agent_transcript_entry_t;

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
    SemaphoreHandle_t state_lock;
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
    char tool_output_bufs[AGENT_MAX_CALLS_PER_TURN][CONFIG_OTOOL_LLM_MAX_TOOL_OUTPUT_BYTES + 1];
    char tool_call_ids[AGENT_MAX_CALLS_PER_TURN][64];
    /* Chat/LOCAL_TRANSCRIPT（WP5） */
    bool chat_mode;
    agent_transcript_entry_t transcript[AGENT_TRANSCRIPT_MAX];
    size_t transcript_count;
    size_t transcript_bytes;
    bool transcript_full; /* 达到上限（§10.2：报 CONTEXT_FULL，不静默删除） */
    /* 循环检测：同一 name+arguments 连续 2 次 → RUN_LIMIT_REACHED（WP4 计划） */
    uint64_t last_tool_fingerprint;
    bool have_last_tool;
};

static void agent_lock(otool_llm_agent_handle_t agent)
{
    xSemaphoreTake(agent->state_lock, portMAX_DELAY);
}

static void agent_unlock(otool_llm_agent_handle_t agent)
{
    xSemaphoreGive(agent->state_lock);
}

static bool agent_is_cancelled(otool_llm_agent_handle_t agent)
{
    bool cancelled;
    agent_lock(agent);
    cancelled = agent->cancel_requested;
    agent_unlock(agent);
    return cancelled;
}

static void agent_request_cancel_from_callback(otool_llm_agent_handle_t agent)
{
    agent_lock(agent);
    agent->cancel_requested = true;
    agent_unlock(agent);
}

static void transcript_entry_free(agent_transcript_entry_t *entry)
{
    if (entry == NULL) {
        return;
    }
    for (size_t i = 0; i < entry->tool_call_count; i++) {
        free((void *)entry->tool_calls[i].id);
        free((void *)entry->tool_calls[i].name);
        free((void *)entry->tool_calls[i].arguments);
    }
    free(entry->tool_calls);
    free(entry->tool_call_id);
    free(entry->text);
    memset(entry, 0, sizeof(*entry));
}

static void transcript_rollback(otool_llm_agent_handle_t agent, size_t keep_count)
{
    while (agent->transcript_count > keep_count) {
        agent_transcript_entry_t *entry = &agent->transcript[--agent->transcript_count];
        if (entry->owned_bytes <= agent->transcript_bytes) {
            agent->transcript_bytes -= entry->owned_bytes;
        } else {
            agent->transcript_bytes = 0;
        }
        transcript_entry_free(entry);
    }
    agent->transcript_full = false;
}

static esp_err_t transcript_reserve(otool_llm_agent_handle_t agent, size_t bytes,
                                    agent_transcript_entry_t **out_entry)
{
    if (agent->transcript_count >= AGENT_TRANSCRIPT_MAX ||
        bytes > CONFIG_OTOOL_LLM_MAX_AGENT_CONTEXT_BYTES - agent->transcript_bytes) {
        agent->transcript_full = true;
        return OTOOL_LLM_ERR_CONTEXT_FULL;
    }
    agent_transcript_entry_t *entry = &agent->transcript[agent->transcript_count];
    memset(entry, 0, sizeof(*entry));
    entry->owned_bytes = bytes;
    *out_entry = entry;
    return ESP_OK;
}

static esp_err_t transcript_add_text(otool_llm_agent_handle_t agent, otool_llm_role_t role,
                                     const char *text, size_t text_len,
                                     const char *tool_call_id)
{
    const char *safe_text = text != NULL ? text : "";
    size_t call_id_len = tool_call_id != NULL ? strlen(tool_call_id) : 0;
    size_t bytes = text_len + 1 + (tool_call_id != NULL ? call_id_len + 1 : 0);
    agent_transcript_entry_t *entry = NULL;
    esp_err_t err = transcript_reserve(agent, bytes, &entry);
    if (err != ESP_OK) {
        return err;
    }
    entry->text = (char *)malloc(text_len + 1);
    if (entry->text == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(entry->text, safe_text, text_len);
    entry->text[text_len] = '\0';
    if (tool_call_id != NULL) {
        entry->tool_call_id = strdup(tool_call_id);
        if (entry->tool_call_id == NULL) {
            transcript_entry_free(entry);
            return ESP_ERR_NO_MEM;
        }
    }
    entry->role = role;
    agent->transcript_bytes += bytes;
    agent->transcript_count++;
    return ESP_OK;
}

static esp_err_t transcript_add_tool_calls(otool_llm_agent_handle_t agent,
                                           const char *text, size_t text_len,
                                           const agent_tool_call_t *calls, size_t call_count)
{
    size_t ready_count = 0;
    size_t bytes = text_len + 1;
    for (size_t i = 0; i < call_count; i++) {
        if (!calls[i].used || !calls[i].ready) {
            continue;
        }
        ready_count++;
        bytes += strlen(calls[i].call_id[0] ? calls[i].call_id : "call_unknown") + 1;
        bytes += strlen(calls[i].name) + 1;
        bytes += calls[i].arguments_len + 1;
    }
    if (ready_count > SIZE_MAX / sizeof(otool_llm_tool_call_msg_t)) {
        return ESP_ERR_NO_MEM;
    }
    bytes += ready_count * sizeof(otool_llm_tool_call_msg_t);
    agent_transcript_entry_t *entry = NULL;
    esp_err_t err = transcript_reserve(agent, bytes, &entry);
    if (err != ESP_OK) {
        return err;
    }
    entry->text = (char *)malloc(text_len + 1);
    entry->tool_calls = (otool_llm_tool_call_msg_t *)calloc(ready_count,
                                                            sizeof(*entry->tool_calls));
    if (entry->text == NULL || (ready_count > 0 && entry->tool_calls == NULL)) {
        transcript_entry_free(entry);
        return ESP_ERR_NO_MEM;
    }
    if (text_len > 0) {
        memcpy(entry->text, text, text_len);
    }
    entry->text[text_len] = '\0';
    size_t dst = 0;
    for (size_t i = 0; i < call_count; i++) {
        if (!calls[i].used || !calls[i].ready) {
            continue;
        }
        entry->tool_calls[dst].id = strdup(calls[i].call_id[0] ? calls[i].call_id
                                                               : "call_unknown");
        entry->tool_calls[dst].name = strdup(calls[i].name);
        entry->tool_calls[dst].arguments = strdup(calls[i].arguments_len > 0
                                                      ? calls[i].arguments
                                                      : "{}");
        if (entry->tool_calls[dst].id == NULL || entry->tool_calls[dst].name == NULL ||
            entry->tool_calls[dst].arguments == NULL) {
            entry->tool_call_count = ready_count;
            transcript_entry_free(entry);
            return ESP_ERR_NO_MEM;
        }
        dst++;
    }
    entry->role = OTOOL_LLM_ROLE_ASSISTANT;
    entry->tool_call_count = ready_count;
    agent->transcript_bytes += bytes;
    agent->transcript_count++;
    return ESP_OK;
}

static uint64_t tool_fingerprint(const char *name, const char *arguments, size_t arguments_len)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    const unsigned char *p = (const unsigned char *)name;
    while (*p != '\0') {
        hash = (hash ^ *p++) * UINT64_C(1099511628211);
    }
    hash = (hash ^ 0xffu) * UINT64_C(1099511628211);
    p = (const unsigned char *)arguments;
    for (size_t i = 0; i < arguments_len; i++) {
        hash = (hash ^ p[i]) * UINT64_C(1099511628211);
    }
    return hash;
}

/* ---------------- 事件发射 ---------------- */

static void agent_emit(otool_llm_agent_handle_t agent, otool_llm_agent_event_t *evt)
{
    if (agent->callback != NULL) {
        otool_llm_event_action_t action = agent->callback(evt, agent->user_ctx);
        if (action == OTOOL_LLM_EVENT_ACTION_CANCEL) {
            agent_request_cancel_from_callback(agent);
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
    bool turn_incomplete;
    esp_err_t turn_error;
    otool_llm_usage_t usage;
    bool usage_set;
    /* 本轮完整文本累积（transcript 需要） */
    char *text;
    size_t text_len;
    size_t text_capacity;
    bool text_overflow;
} bridge_ctx_t;

static void bridge_append_text(bridge_ctx_t *b, const char *data, size_t len)
{
    if (!b->agent->chat_mode || len == 0 || b->text_overflow) {
        return;
    }
    if (len > CONFIG_OTOOL_LLM_MAX_AGENT_CONTEXT_BYTES - b->text_len) {
        b->text_overflow = true;
        return;
    }
    size_t needed = b->text_len + len + 1;
    if (needed > b->text_capacity) {
        size_t next = b->text_capacity > 0 ? b->text_capacity : 256;
        while (next < needed && next <= CONFIG_OTOOL_LLM_MAX_AGENT_CONTEXT_BYTES / 2) {
            next *= 2;
        }
        if (next < needed) {
            next = needed;
        }
        char *grown = (char *)realloc(b->text, next);
        if (grown == NULL) {
            b->text_overflow = true;
            return;
        }
        b->text = grown;
        b->text_capacity = next;
    }
    memcpy(b->text + b->text_len, data, len);
    b->text_len += len;
    b->text[b->text_len] = '\0';
}

static agent_tool_call_t *bridge_find_call(bridge_ctx_t *b, uint32_t output_index,
                                           int *slot_out)
{
    for (int i = 0; i < b->call_count; i++) {
        if (b->calls[i].used && b->calls[i].output_index == output_index) {
            if (slot_out != NULL) {
                *slot_out = i;
            }
            return &b->calls[i];
        }
    }
    return NULL;
}

static bool bridge_copy_field(char *dst, size_t capacity, const char *src)
{
    if (src == NULL) {
        return true;
    }
    size_t len = strlen(src);
    if (len >= capacity) {
        return false;
    }
    memcpy(dst, src, len + 1);
    return true;
}

static otool_llm_event_action_t bridge_cb(const otool_llm_text_event_t *evt, void *user_ctx)
{
    bridge_ctx_t *b = (bridge_ctx_t *)user_ctx;
    otool_llm_agent_handle_t agent = b->agent;

    switch (evt->type) {
    case OTOOL_LLM_TEXT_EVENT_RESPONSE_STARTED:
        if (evt->response_id != NULL &&
            !bridge_copy_field(b->response_id, sizeof(b->response_id), evt->response_id)) {
            b->turn_error = OTOOL_LLM_ERR_PROTOCOL;
        }
        break;
    case OTOOL_LLM_TEXT_EVENT_TEXT_DELTA: {
        otool_llm_agent_event_t ae = { .type = OTOOL_LLM_AGENT_EVENT_TEXT_DELTA };
        ae.turn_index = agent->turn_index;
        ae.data.text_delta.data = evt->data.text_delta.data;
        ae.data.text_delta.data_len = evt->data.text_delta.data_len;
        agent_emit(agent, &ae);
        bridge_append_text(b, evt->data.text_delta.data, evt->data.text_delta.data_len);
        break;
    }
    case OTOOL_LLM_TEXT_EVENT_TOOL_CALL_STARTED: {
        int slot = -1;
        agent_tool_call_t *c = bridge_find_call(b, evt->data.tool_call_started.output_index, &slot);
        if (c == NULL && b->call_count < AGENT_MAX_CALLS_PER_TURN) {
            slot = b->call_count++;
            c = &b->calls[slot];
            memset(c, 0, sizeof(*c));
            c->used = true;
            c->output_index = evt->data.tool_call_started.output_index;
        }
        if (c == NULL ||
            !bridge_copy_field(c->call_id, sizeof(c->call_id),
                               evt->data.tool_call_started.call_id) ||
            !bridge_copy_field(c->name, sizeof(c->name), evt->data.tool_call_started.name)) {
            b->turn_error = OTOOL_LLM_ERR_PROTOCOL;
        } else {
            otool_llm_agent_event_t ae = { .type = OTOOL_LLM_AGENT_EVENT_TOOL_CALL_STARTED };
            ae.turn_index = agent->turn_index;
            ae.tool_index = (uint32_t)slot;
            ae.call_id = c->call_id[0] ? c->call_id : NULL;
            ae.name = c->name[0] ? c->name : NULL;
            agent_emit(agent, &ae);
        }
        break;
    }
    case OTOOL_LLM_TEXT_EVENT_TOOL_ARGUMENTS_DELTA: {
        int slot = -1;
        agent_tool_call_t *c = bridge_find_call(b, evt->data.tool_arguments_delta.output_index,
                                                &slot);
        if (c != NULL) {
            size_t dlen = evt->data.tool_arguments_delta.delta_len;
            if (c->arguments_len + dlen < sizeof(c->arguments)) {
                memcpy(c->arguments + c->arguments_len, evt->data.tool_arguments_delta.delta, dlen);
                c->arguments_len += dlen;
                c->arguments[c->arguments_len] = '\0';
            } else {
                b->turn_error = OTOOL_LLM_ERR_TOOL_ARGUMENTS;
            }
            otool_llm_agent_event_t ae = { .type = OTOOL_LLM_AGENT_EVENT_TOOL_ARGUMENTS_DELTA };
            ae.turn_index = agent->turn_index;
            ae.tool_index = (uint32_t)slot;
            ae.call_id = c->call_id[0] ? c->call_id : NULL;
            ae.data.tool_arguments_delta.delta = evt->data.tool_arguments_delta.delta;
            ae.data.tool_arguments_delta.delta_len = dlen;
            agent_emit(agent, &ae);
        } else {
            b->turn_error = OTOOL_LLM_ERR_PROTOCOL;
        }
        break;
    }
    case OTOOL_LLM_TEXT_EVENT_TOOL_CALL_DONE: {
        int slot = -1;
        agent_tool_call_t *c = bridge_find_call(b, evt->data.tool_call_done.output_index, &slot);
        if (c != NULL) {
            /* 用事件中的完整 arguments 覆盖（SDK 已校验与 delta 一致） */
            if (evt->data.tool_call_done.arguments != NULL &&
                evt->data.tool_call_done.arguments_len < sizeof(c->arguments)) {
                memcpy(c->arguments, evt->data.tool_call_done.arguments,
                       evt->data.tool_call_done.arguments_len);
                c->arguments_len = evt->data.tool_call_done.arguments_len;
                c->arguments[c->arguments_len] = '\0';
            } else if (evt->data.tool_call_done.arguments != NULL) {
                b->turn_error = OTOOL_LLM_ERR_TOOL_ARGUMENTS;
                break;
            }
            if (!bridge_copy_field(c->call_id, sizeof(c->call_id),
                                   evt->data.tool_call_done.call_id) ||
                !bridge_copy_field(c->name, sizeof(c->name), evt->data.tool_call_done.name)) {
                b->turn_error = OTOOL_LLM_ERR_PROTOCOL;
                break;
            }
            c->ready = true;
            otool_llm_agent_event_t ae = { .type = OTOOL_LLM_AGENT_EVENT_TOOL_CALL_READY };
            ae.turn_index = agent->turn_index;
            ae.tool_index = (uint32_t)slot;
            ae.call_id = c->call_id[0] ? c->call_id : NULL;
            ae.name = c->name[0] ? c->name : NULL;
            ae.data.tool_call_ready.arguments = c->arguments;
            ae.data.tool_call_ready.arguments_len = c->arguments_len;
            agent_emit(agent, &ae);
        } else {
            b->turn_error = OTOOL_LLM_ERR_PROTOCOL;
        }
        break;
    }
    case OTOOL_LLM_TEXT_EVENT_USAGE:
        b->usage = evt->data.usage;
        b->usage_set = true;
        break;
    case OTOOL_LLM_TEXT_EVENT_COMPLETED:
        b->turn_ok = true;
        if (b->usage_set) {
            otool_llm_agent_event_t ae = { .type = OTOOL_LLM_AGENT_EVENT_USAGE };
            ae.turn_index = agent->turn_index;
            ae.data.usage = b->usage;
            agent_emit(agent, &ae);
        }
        break;
    case OTOOL_LLM_TEXT_EVENT_INCOMPLETE:
        b->turn_ok = true;
        b->turn_incomplete = true;
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
        agent_request_cancel_from_callback(agent);
        break;
    default:
        break;
    }
    return agent_is_cancelled(agent) ? OTOOL_LLM_EVENT_ACTION_CANCEL
                                     : OTOOL_LLM_EVENT_ACTION_CONTINUE;
}

/* ---------------- 工具执行 ---------------- */

static bool utf8_valid(const char *data, size_t len)
{
    const unsigned char *s = (const unsigned char *)data;
    size_t i = 0;
    while (i < len) {
        unsigned char c = s[i++];
        if (c <= 0x7f) {
            continue;
        }
        size_t continuation = 0;
        uint32_t value = 0;
        if ((c & 0xe0u) == 0xc0u) {
            continuation = 1;
            value = c & 0x1fu;
            if (value < 2) {
                return false; /* overlong */
            }
        } else if ((c & 0xf0u) == 0xe0u) {
            continuation = 2;
            value = c & 0x0fu;
        } else if ((c & 0xf8u) == 0xf0u) {
            continuation = 3;
            value = c & 0x07u;
        } else {
            return false;
        }
        if (continuation > len - i) {
            return false;
        }
        for (size_t j = 0; j < continuation; j++) {
            unsigned char cc = s[i++];
            if ((cc & 0xc0u) != 0x80u) {
                return false;
            }
            value = (value << 6) | (cc & 0x3fu);
        }
        if ((continuation == 2 && value < 0x800u) ||
            (continuation == 3 && value < 0x10000u) || value > 0x10ffffu ||
            (value >= 0xd800u && value <= 0xdfffu)) {
            return false;
        }
    }
    return true;
}

static bool tool_output_is_json_object(const char *output, size_t output_len)
{
    if (!utf8_valid(output, output_len) || memchr(output, '\0', output_len) != NULL) {
        return false;
    }
    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(output, output_len + 1, &parse_end, true);
    bool ok = root != NULL && cJSON_IsObject(root);
    cJSON_Delete(root);
    return ok;
}

static esp_err_t agent_execute_tool(otool_llm_agent_handle_t agent,
                                    const otool_llm_tool_definition_t *tool,
                                    const char *arguments, size_t arguments_len,
                                    char *output, size_t output_cap, size_t *output_len,
                                    int64_t run_deadline_us)
{
    /* 授权检查在 run_stream 工具循环中完成（policy/SIDE_EFFECTING/NEEDS_APPROVAL）；
     * 这里只执行。 */
    size_t max_output = tool->max_output_bytes > 0 ? tool->max_output_bytes
                                                   : CONFIG_OTOOL_LLM_MAX_TOOL_OUTPUT_BYTES;
    if (max_output > CONFIG_OTOOL_LLM_MAX_TOOL_OUTPUT_BYTES || output_cap <= max_output) {
        tool_error_output("tool_output_too_large", "invalid output budget", false, output,
                          output_cap);
        *output_len = strlen(output);
        return OTOOL_LLM_ERR_TOOL_OUTPUT_TOO_LARGE;
    }
    if (tool->execute == NULL) {
        tool_error_output("tool_failed", "tool has no executor", false, output, output_cap);
        *output_len = strlen(output);
        return OTOOL_LLM_ERR_TOOL_FAILED;
    }

    uint32_t timeout = tool->timeout_ms > 0 ? tool->timeout_ms : CONFIG_OTOOL_LLM_DEFAULT_TOOL_TIMEOUT_MS;
    int64_t tool_deadline = esp_timer_get_time() + (int64_t)timeout * 1000;
    if (run_deadline_us > 0 && tool_deadline > run_deadline_us) {
        tool_deadline = run_deadline_us;
    }
    otool_llm_tool_exec_context_t exec_ctx = {
        .cancel_requested = &agent->cancel_requested,
        .deadline_us = tool_deadline,
    };
    output[0] = '\0';
    output[max_output] = '\0';
    *output_len = 0;
    esp_err_t err = tool->execute(arguments, output, max_output + 1, output_len, &exec_ctx,
                                  tool->user_ctx);
    if (err != ESP_OK) {
        if (agent_is_cancelled(agent)) {
            tool_error_output("cancelled", "cancelled", false, output, output_cap);
        } else {
            tool_error_output("tool_failed", "tool execution failed", true, output, output_cap);
        }
        *output_len = strlen(output);
        return err;
    }
    if (*output_len > max_output || output[*output_len] != '\0') {
        tool_error_output("tool_output_too_large", "tool output exceeded its byte budget", false,
                          output, output_cap);
        *output_len = strlen(output);
        return OTOOL_LLM_ERR_TOOL_OUTPUT_TOO_LARGE;
    }
    if (!tool_output_is_json_object(output, *output_len)) {
        tool_error_output("tool_failed", "tool output must be one UTF-8 JSON object", false,
                          output, output_cap);
        *output_len = strlen(output);
        return OTOOL_LLM_ERR_TOOL_FAILED;
    }
    if (esp_timer_get_time() > tool_deadline) {
        tool_error_output("tool_timeout", "tool exceeded its cooperative deadline", true, output,
                          output_cap);
        *output_len = strlen(output);
        return ESP_ERR_TIMEOUT;
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
    agent_lock(agent);
    if (agent->running) {
        agent_unlock(agent);
        return ESP_ERR_INVALID_STATE;
    }
    agent->running = true;
    agent->cancel_requested = false;
    agent->active_request = NULL;
    agent_unlock(agent);
    agent->callback = callback;
    agent->user_ctx = user_ctx;
    agent->turn_index = 0;
    agent->last_output_count = 0;
    agent->have_last_tool = false; /* 循环检测随 run 重置 */

    size_t run_transcript_start = agent->transcript_count;
    bool run_completed = false;

    otool_llm_agent_event_t evt = { .type = OTOOL_LLM_AGENT_EVENT_RUN_STARTED };
    agent_emit(agent, &evt);

    if (agent->chat_mode) {
        esp_err_t transcript_err = transcript_add_text(agent, OTOOL_LLM_ROLE_USER, user_text,
                                                       strlen(user_text), NULL);
        if (transcript_err != ESP_OK) {
            evt.type = OTOOL_LLM_AGENT_EVENT_ERROR;
            evt.data.error.code = transcript_err;
            evt.data.error.message = otool_llm_err_to_name(transcript_err);
            agent_emit(agent, &evt);
            transcript_rollback(agent, run_transcript_start);
            agent_lock(agent);
            agent->running = false;
            agent->callback = NULL;
            agent->user_ctx = NULL;
            agent_unlock(agent);
            return transcript_err;
        }
    }

    int64_t deadline = esp_timer_get_time() + (int64_t)agent->run_timeout_ms * 1000;
    esp_err_t result = ESP_OK;
    bool finished = false;
    bool first_turn = true;
    uint32_t total_tool_calls = 0;

    while (!finished) {
        if (agent_is_cancelled(agent)) {
            evt.type = OTOOL_LLM_AGENT_EVENT_CANCELLED;
            agent_emit(agent, &evt);
            result = ESP_OK;
            break;
        }
        if (agent->transcript_full) {
            /* §10.2：本地 transcript 达到上限 → CONTEXT_FULL，不静默删除中间消息 */
            evt.type = OTOOL_LLM_AGENT_EVENT_ERROR;
            evt.data.error.code = OTOOL_LLM_ERR_CONTEXT_FULL;
            evt.data.error.message = otool_llm_err_to_name(OTOOL_LLM_ERR_CONTEXT_FULL);
            agent_emit(agent, &evt);
            result = OTOOL_LLM_ERR_CONTEXT_FULL;
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

        /* 构建请求（Chat/LOCAL_TRANSCRIPT 与 Responses 链分支） */
        otool_llm_text_message_t msg = { .role = OTOOL_LLM_ROLE_USER, .text = user_text };
        otool_llm_text_message_t chat_msgs[AGENT_TRANSCRIPT_MAX];
        otool_llm_text_request_t req = {};
        req.struct_size = sizeof(req);
        req.model = agent->model;
        req.instructions = agent->instructions;
        req.max_output_tokens = 2048;
        if (agent->chat_mode) {
            /* 本地 transcript 已在 run 入口追加本轮 user；这里精确重建，不重复追加。 */
            size_t n = 0;
            for (size_t i = 0; i < agent->transcript_count && n < AGENT_TRANSCRIPT_MAX; i++) {
                agent_transcript_entry_t *e = &agent->transcript[i];
                chat_msgs[n].role = e->role;
                chat_msgs[n].text = e->text;
                chat_msgs[n].tool_call_id = e->role == OTOOL_LLM_ROLE_TOOL ? e->tool_call_id : NULL;
                chat_msgs[n].tool_calls = NULL;
                chat_msgs[n].tool_call_count = 0;
                if (e->role == OTOOL_LLM_ROLE_ASSISTANT && e->tool_call_count > 0) {
                    chat_msgs[n].tool_calls = e->tool_calls;
                    chat_msgs[n].tool_call_count = e->tool_call_count;
                }
                n++;
            }
            req.messages = chat_msgs;
            req.message_count = n;
        } else {
            req.store = true; /* REMOTE_RESPONSE_CHAIN */
            if (first_turn) {
                req.messages = &msg;
                req.message_count = 1;
                req.previous_response_id =
                    agent->response_id[0] != '\0' ? agent->response_id : NULL;
            } else {
                req.tool_outputs = agent->last_tool_outputs;
                req.tool_output_count = agent->last_output_count;
                req.previous_response_id =
                    agent->response_id[0] != '\0' ? agent->response_id : NULL;
            }
        }
        req.tools = tool_count > 0 ? tools : NULL;
        req.tool_count = tool_count;

        otool_llm_request_handle_t request = NULL;
        esp_err_t err = otool_llm_request_create(agent->client, &req, &request);
        if (err != ESP_OK) {
            evt.type = OTOOL_LLM_AGENT_EVENT_ERROR;
            evt.data.error.code = err;
            evt.data.error.message = otool_llm_err_to_name(err);
            agent_emit(agent, &evt);
            result = err;
            break;
        }
        agent_lock(agent);
        agent->active_request = request;
        bool cancel_before_execute = agent->cancel_requested;
        if (cancel_before_execute) {
            /* state_lock pins request lifetime until cancel() returns. */
            otool_llm_request_cancel(request);
        }
        agent_unlock(agent);

        bridge_ctx_t bridge = { 0 };
        bridge.agent = agent;
        err = otool_llm_request_execute_stream(request, bridge_cb, &bridge);
        /* Clear only after any cross-task cancel holding state_lock has returned. */
        agent_lock(agent);
        agent->active_request = NULL;
        agent_unlock(agent);
        otool_llm_request_destroy(request);

        if (agent_is_cancelled(agent)) {
            free(bridge.text);
            evt.type = OTOOL_LLM_AGENT_EVENT_CANCELLED;
            agent_emit(agent, &evt);
            result = ESP_OK;
            break;
        }
        if (bridge.text_overflow) {
            free(bridge.text);
            evt.type = OTOOL_LLM_AGENT_EVENT_ERROR;
            evt.data.error.code = OTOOL_LLM_ERR_CONTEXT_FULL;
            evt.data.error.message = otool_llm_err_to_name(OTOOL_LLM_ERR_CONTEXT_FULL);
            agent_emit(agent, &evt);
            result = OTOOL_LLM_ERR_CONTEXT_FULL;
            break;
        }
        if (bridge.turn_error != ESP_OK || (err != ESP_OK && !bridge.turn_ok)) {
            esp_err_t turn_err = bridge.turn_error != ESP_OK ? bridge.turn_error : err;
            free(bridge.text);
            evt.type = OTOOL_LLM_AGENT_EVENT_ERROR;
            evt.data.error.code = turn_err != ESP_OK ? turn_err : OTOOL_LLM_ERR_PROTOCOL;
            evt.data.error.message = otool_llm_err_to_name(evt.data.error.code);
            agent_emit(agent, &evt);
            result = evt.data.error.code;
            break;
        }
        if (bridge.response_id[0] != '\0') {
            memcpy(agent->response_id, bridge.response_id, strlen(bridge.response_id) + 1);
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

        if (agent_is_cancelled(agent)) {
            free(bridge.text);
            evt.type = OTOOL_LLM_AGENT_EVENT_CANCELLED;
            agent_emit(agent, &evt);
            result = ESP_OK;
            break;
        }

        if (bridge.turn_incomplete) {
            free(bridge.text);
            evt.type = OTOOL_LLM_AGENT_EVENT_RUN_LIMIT_REACHED;
            agent_emit(agent, &evt);
            result = ESP_OK;
            break;
        }

        if (ready_count == 0) {
            /* 最终回答轮：Chat 模式把模型文本记入 transcript */
            if (agent->chat_mode && bridge.text_len > 0) {
                esp_err_t transcript_err = transcript_add_text(
                    agent, OTOOL_LLM_ROLE_ASSISTANT, bridge.text, bridge.text_len, NULL);
                if (transcript_err != ESP_OK) {
                    free(bridge.text);
                    evt.type = OTOOL_LLM_AGENT_EVENT_ERROR;
                    evt.data.error.code = transcript_err;
                    evt.data.error.message = otool_llm_err_to_name(transcript_err);
                    agent_emit(agent, &evt);
                    result = transcript_err;
                    break;
                }
            }
            free(bridge.text);
            evt.type = OTOOL_LLM_AGENT_EVENT_RUN_COMPLETED;
            agent_emit(agent, &evt);
            result = ESP_OK;
            run_completed = true;
            break;
        }

        /* Chat 模式：模型发出工具调用 → 记录 assistant(tool_calls) 消息 */
        if (agent->chat_mode) {
            esp_err_t transcript_err = transcript_add_tool_calls(
                agent, bridge.text != NULL ? bridge.text : "", bridge.text_len, bridge.calls,
                (size_t)bridge.call_count);
            if (transcript_err != ESP_OK) {
                free(bridge.text);
                evt.type = OTOOL_LLM_AGENT_EVENT_ERROR;
                evt.data.error.code = transcript_err;
                evt.data.error.message = otool_llm_err_to_name(transcript_err);
                agent_emit(agent, &evt);
                result = transcript_err;
                break;
            }
        }
        free(bridge.text);

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
            /* 循环检测：与上一轮同一工具同一参数 → 模型在重复调用，停止（WP4） */
            uint64_t fingerprint = tool_fingerprint(c->name, c->arguments, c->arguments_len);
            if (agent->have_last_tool && agent->last_tool_fingerprint == fingerprint) {
                ESP_LOGW(TAG, "tool loop detected (%s), stopping run", c->name);
                evt.type = OTOOL_LLM_AGENT_EVENT_RUN_LIMIT_REACHED;
                agent_emit(agent, &evt);
                finished = true;
                break;
            }
            agent->last_tool_fingerprint = fingerprint;
            agent->have_last_tool = true;
            total_tool_calls++;
            agent->tool_calls_done = total_tool_calls;

            evt.type = OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_STARTED;
            evt.turn_index = agent->turn_index;
            evt.tool_index = (uint32_t)i;
            evt.call_id = c->call_id[0] ? c->call_id : NULL;
            evt.name = c->name[0] ? c->name : NULL;
            agent_emit(agent, &evt);

            /* A callback cancellation is authoritative: never cross the execution boundary. */
            if (agent_is_cancelled(agent)) {
                break;
            }

            char output[CONFIG_OTOOL_LLM_MAX_TOOL_OUTPUT_BYTES + 1];
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
                                                            sizeof(output), &output_len, deadline);
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
                memcpy(agent->tool_output_bufs[out_count], output, output_len + 1);
                out_item->call_id = agent->tool_call_ids[out_count];
                out_item->output = agent->tool_output_bufs[out_count];
                out_count++;
                /* Chat 模式：追加 tool 结果消息到 transcript */
                if (agent->chat_mode) {
                    esp_err_t transcript_err = transcript_add_text(
                        agent, OTOOL_LLM_ROLE_TOOL, output, output_len,
                        c->call_id[0] ? c->call_id : "call_unknown");
                    if (transcript_err != ESP_OK) {
                        evt.type = OTOOL_LLM_AGENT_EVENT_ERROR;
                        evt.data.error.code = transcript_err;
                        evt.data.error.message = otool_llm_err_to_name(transcript_err);
                        agent_emit(agent, &evt);
                        result = transcript_err;
                        finished = true;
                        break;
                    }
                }
            }
            if (agent_is_cancelled(agent)) {
                break;
            }
        }
        if (agent_is_cancelled(agent)) {
            evt.type = OTOOL_LLM_AGENT_EVENT_CANCELLED;
            agent_emit(agent, &evt);
            result = ESP_OK;
            finished = true;
        }
        agent->last_output_count = out_count;
        first_turn = false;
    }

    if (agent->chat_mode && !run_completed) {
        transcript_rollback(agent, run_transcript_start);
    }
    agent_lock(agent);
    agent->running = false;
    agent->callback = NULL;
    agent->user_ctx = NULL;
    agent_unlock(agent);
    return result;
}

esp_err_t otool_llm_agent_cancel(otool_llm_agent_handle_t agent)
{
    if (agent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    agent_lock(agent);
    agent->cancel_requested = true;
    if (agent->active_request != NULL) {
        /* Holding state_lock pins active_request until request_cancel() returns. */
        otool_llm_request_cancel(agent->active_request);
    }
    agent_unlock(agent);
    return ESP_OK;
}

void otool_llm_agent_reset_session(otool_llm_agent_handle_t agent)
{
    if (agent == NULL) {
        return;
    }
    agent_lock(agent);
    if (agent->running) {
        ESP_LOGE(TAG, "refusing to reset session while a run is active");
        agent_unlock(agent);
        return;
    }
    agent->response_id[0] = '\0';
    agent->last_output_count = 0;
    transcript_rollback(agent, 0);
    agent_unlock(agent);
}

void otool_llm_agent_destroy(otool_llm_agent_handle_t agent)
{
    if (agent == NULL) {
        return;
    }
    agent_lock(agent);
    if (agent->running) {
        ESP_LOGE(TAG, "refusing to destroy agent while a run is active");
        agent_unlock(agent);
        return;
    }
    transcript_rollback(agent, 0);
    free(agent->instructions);
    free(agent->model);
    SemaphoreHandle_t state_lock = agent->state_lock;
    agent_unlock(agent);
    vSemaphoreDelete(state_lock);
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
    if (!otool_llm_tool_registry_is_sealed(config->tools)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (config->parallel_tool_calls) {
        return OTOOL_LLM_ERR_UNSUPPORTED;
    }
    otool_llm_agent_handle_t agent = (otool_llm_agent_handle_t)calloc(1, sizeof(*agent));
    if (agent == NULL) {
        return ESP_ERR_NO_MEM;
    }
    agent->state_lock = xSemaphoreCreateMutex();
    if (agent->state_lock == NULL) {
        free(agent);
        return ESP_ERR_NO_MEM;
    }
    agent->client = config->client;
    agent->tools = config->tools;
    agent->state_mode = config->state_mode;
    agent->chat_mode = (config->state_mode == OTOOL_LLM_AGENT_STATE_LOCAL_TRANSCRIPT);
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
        vSemaphoreDelete(agent->state_lock);
        free(agent);
        return ESP_ERR_NO_MEM;
    }
    agent->model = model;
    agent->instructions = instructions;

    *out_agent = agent;
    return ESP_OK;
}
