# otool_llm_sdk

ESP-IDF C component for bounded streaming text and basic tool-enabled agents.

- Ark/OpenAI/custom provider presets; OpenAI provider remains available.
- Responses SSE and Chat Completions SSE adapters.
- Bounded SSE, request, tool-schema, argument, output and transcript memory.
- Sequential function calling with local schema validation and side-effect policy.
- Responses remote chains and persistent Chat transcripts across agent runs.
- Cross-task request/agent cancellation with one terminal event per run.
- HTTPS and certificate bundle enforcement by default.

Version 0.3.0 focuses on the common Agent runtime and Ark-compatible paths. OpenAI
live validation is intentionally deferred until test credentials are available;
the provider and protocol code are not removed.

## Streaming text

```c
#include "otool_llm_sdk.h"
#include "otool_llm_text.h"

static otool_llm_event_action_t on_text(const otool_llm_text_event_t *event, void *ctx)
{
    if (event->type == OTOOL_LLM_TEXT_EVENT_TEXT_DELTA) {
        printf("%.*s", (int)event->data.text_delta.data_len,
               event->data.text_delta.data);
    }
    return OTOOL_LLM_EVENT_ACTION_CONTINUE;
}

void stream_once(const char *runtime_key)
{
    otool_llm_client_config_t config = {
        .struct_size = sizeof(config),
        .provider = OTOOL_LLM_PROVIDER_VOLCENGINE_ARK,
        .protocol = OTOOL_LLM_PROTOCOL_AUTO,
        .api_key = runtime_key,
    };
    otool_llm_client_handle_t client = NULL;
    ESP_ERROR_CHECK(otool_llm_client_create(&config, &client));

    otool_llm_text_message_t message = {
        .role = OTOOL_LLM_ROLE_USER,
        .text = "你好",
    };
    otool_llm_text_request_t input = {
        .struct_size = sizeof(input),
        .model = "your-endpoint-or-model-id",
        .messages = &message,
        .message_count = 1,
    };
    otool_llm_request_handle_t request = NULL;
    ESP_ERROR_CHECK(otool_llm_request_create(client, &input, &request));
    esp_err_t result = otool_llm_request_execute_stream(request, on_text, NULL);
    otool_llm_request_destroy(request);
    otool_llm_client_destroy(client);
    ESP_ERROR_CHECK(result);
}
```

## Basic Agent and tools

Register tools, seal the registry, then create one Agent. Tool callbacks run
synchronously on the Agent worker task.

```c
#include "otool_llm_agent.h"

static otool_llm_event_action_t on_agent_event(
    const otool_llm_agent_event_t *event, void *user_ctx)
{
    (void)user_ctx;
    if (event->type == OTOOL_LLM_AGENT_EVENT_TEXT_DELTA) {
        printf("%.*s", (int)event->data.text_delta.data_len,
               event->data.text_delta.data);
    }
    return OTOOL_LLM_EVENT_ACTION_CONTINUE;
}

static esp_err_t read_status(const char *arguments, char *output, size_t capacity,
                             size_t *output_len,
                             const otool_llm_tool_exec_context_t *exec_ctx,
                             void *user_ctx)
{
    (void)arguments;
    (void)exec_ctx;
    (void)user_ctx;
    int written = snprintf(output, capacity, "{\"ok\":true,\"result\":{\"ready\":true}}");
    if (written < 0 || (size_t)written >= capacity) {
        return OTOOL_LLM_ERR_TOOL_OUTPUT_TOO_LARGE;
    }
    *output_len = (size_t)written;
    return ESP_OK;
}

otool_llm_tool_registry_handle_t registry = NULL;
ESP_ERROR_CHECK(otool_llm_tool_registry_create(&registry));
otool_llm_tool_definition_t tool = {
    .struct_size = sizeof(tool),
    .name = "read_status",
    .description = "Read device status",
    .parameters_json_schema =
        "{\"type\":\"object\",\"properties\":{},\"required\":[],"
        "\"additionalProperties\":false}",
    .strict = true,
    .flags = OTOOL_LLM_TOOL_READ_ONLY,
    .timeout_ms = 5000,
    .max_output_bytes = 512,
    .execute = read_status,
};
ESP_ERROR_CHECK(otool_llm_tool_registry_add(registry, &tool));
ESP_ERROR_CHECK(otool_llm_tool_registry_seal(registry));

otool_llm_agent_config_t agent_config = {
    .struct_size = sizeof(agent_config),
    .client = client,
    .tools = registry,
    .model = "your-endpoint-or-model-id",
    .instructions = "Be concise.",
    .state_mode = OTOOL_LLM_AGENT_STATE_REMOTE_RESPONSE_CHAIN,
    .parallel_tool_calls = false,
};
otool_llm_agent_handle_t agent = NULL;
ESP_ERROR_CHECK(otool_llm_agent_create(&agent_config, &agent));
ESP_ERROR_CHECK(otool_llm_agent_run_stream(agent, "检查设备", on_agent_event, NULL));
```

Strict schemas must explicitly use `additionalProperties:false` and list every
declared property in `required`. Non-strict schemas may leave properties
optional. Nested objects/arrays and unsupported schema keywords are rejected
at registration. Model arguments are validated again immediately before tool
execution.

A successful tool callback must return one NUL-terminated UTF-8 JSON object and
an exact byte length. The SDK rejects invalid JSON, missing termination and
outputs beyond either the per-tool or global budget. Side-effecting tools are
denied unless the Agent policy callback explicitly allows them. Returning
`CANCEL` from `TOOL_EXECUTION_STARTED` prevents the executor from running.

`REMOTE_RESPONSE_CHAIN` sends the previous response id on the first request of
the next run. `LOCAL_TRANSCRIPT` retains complete user/assistant/tool messages
until its count or byte budget is exhausted. Call `otool_llm_agent_reset_session()`
between conversations.

## Threading and lifetime

- Execute requests and Agents on worker tasks, never the LVGL task.
- Text and Agent callbacks run on the executing worker task.
- Request and Agent cancellation are idempotent and callable from another task.
- One run is allowed per Agent; `parallel_tool_calls=true` is rejected.
- The client and sealed registry must outlive the Agent.
- Destroy requests only after execution returns; destroy Agents only while idle.
- Tool deadlines are cooperative. A callback must poll `cancel_requested` and
  `deadline_us`; the SDK also rejects a late result after the callback returns.

## Security

- API keys are deep-copied at client creation and securely zeroed at destroy.
- Do not hard-code credentials in tracked source or print prompts, arguments,
  outputs, request bodies, authorization headers or provider keys.
- HTTPS is mandatory unless `OTOOL_LLM_ALLOW_INSECURE_HTTP` is enabled for a
  controlled local transport test.
- NVS encryption is an application/deployment concern. The sample application
  warns when it is disabled; production firmware must provision encrypted NVS
  or full flash encryption according to its manufacturing flow.

### Sample application credentials

The Tab5 sample intentionally supports deployment credentials through the local
`sdkconfig` file. Run `idf.py menuconfig`, open **otool_tab5_live2d app**, and set:

- `OTOOL_WIFI_SSID`
- `OTOOL_WIFI_PASSWORD`
- `OTOOL_LLM_API_KEY`

All repository defaults are empty and `sdkconfig` is Git-ignored. A non-empty
local `sdkconfig` value takes precedence over the optional legacy NVS fallback.
An empty SSID starts the application in offline mode; an empty API key disables
the LLM and Agent workers. Neither case aborts the application, so the display
and console remain available.

This choice embeds configured values in the firmware image. Do not distribute a
configured `sdkconfig`, build directory, firmware image, or core dump as if it
were secret-free; production deployments should add flash encryption and their
own provisioning/rotation policy.

## Important Kconfig budgets

| Option | Default | Meaning |
| --- | ---: | --- |
| `OTOOL_LLM_MAX_REQUEST_BYTES` | 32768 | Serialized request body |
| `OTOOL_LLM_MAX_REQUEST_MESSAGES` | 32 | Messages accepted by one direct text request |
| `OTOOL_LLM_MAX_BASE_URL_BYTES` | 512 | Provider base URL copied by a client |
| `OTOOL_LLM_MAX_ENDPOINT_PATH_BYTES` | 256 | Custom Responses/Chat endpoint path |
| `OTOOL_LLM_MAX_API_KEY_BYTES` | 896 | Runtime API key kept within the auth-header budget |
| `OTOOL_LLM_MAX_SSE_EVENT_BYTES` | 16384 | One merged SSE event |
| `OTOOL_LLM_MAX_TOOLS` | 8 | Tools in a registry/request |
| `OTOOL_LLM_MAX_TOOL_NAME_BYTES` | 64 | One ASCII tool name |
| `OTOOL_LLM_MAX_TOOL_DESCRIPTION_BYTES` | 512 | One model-facing description |
| `OTOOL_LLM_MAX_TOOL_SCHEMA_BYTES` | 2048 | One tool schema |
| `OTOOL_LLM_MAX_TOTAL_TOOL_SCHEMA_BYTES` | 8192 | Aggregate registry schemas |
| `OTOOL_LLM_MAX_TOOL_ARGUMENT_BYTES` | 4096 | One model-generated argument object |
| `OTOOL_LLM_MAX_TOOL_OUTPUT_BYTES` | 4096 | One tool result |
| `OTOOL_LLM_MAX_AGENT_MESSAGES` | 24 | Local Chat transcript entries |
| `OTOOL_LLM_MAX_AGENT_CONTEXT_BYTES` | 32768 | Owned Chat transcript bytes |
| `OTOOL_LLM_DEFAULT_AGENT_TIMEOUT_MS` | 120000 | Whole run deadline |
| `OTOOL_LLM_DEFAULT_TOOL_TIMEOUT_MS` | 15000 | Cooperative tool deadline |

All caps fail explicitly; content is never silently truncated.

## Tests

From the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File components/otool_llm_sdk/test_apps/run_host_tests.ps1
```

The host runner builds parser/provider/schema and Agent tests against the
vendored cJSON test dependency. The ESP-IDF `transport_test` app covers the
HTTP/SSE transport matrix separately.

Compile the standalone transport app for the device target:

```powershell
idf.py -C components/otool_llm_sdk/test_apps/transport_test `
  -B build-transport -D IDF_TARGET=esp32p4 build
```

To execute its local HTTP/SSE matrix, run `test_apps/transport_test/run_wsl.sh`
inside a Linux/WSL ESP-IDF environment. The script starts and later stops only
its own fixture server process, builds the Linux target and returns the test
ELF's exit code.
