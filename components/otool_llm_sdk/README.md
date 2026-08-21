# otool_llm_sdk

ESP-IDF component for streaming LLM text with a bounded SSE pipeline.

- Primary protocol: **OpenAI Responses API** (`POST /responses` + SSE)
- Fallback protocol: **Chat Completions SSE** (`POST /chat/completions` + SSE)
- Provider presets: OpenAI, Volcano Ark, Custom (OpenAI-compatible)
- Bounded byte-level SSE parser (CRLF/LF/CR, multi-line data, UTF-8 at any split)
- Cross-task cancel, exactly one terminal event per request
- HTTPS + certificate bundle enforced; no secret in logs

## Usage

Add `otool_llm_sdk` to your component's `REQUIRES`, then:

```c
#include "otool_llm_sdk.h"
#include "otool_llm_text.h"

static otool_llm_event_action_t on_event(const otool_llm_text_event_t *evt, void *user_ctx)
{
    switch (evt->type) {
    case OTOOL_LLM_TEXT_EVENT_TEXT_DELTA:
        printf("%.*s", (int)evt->data.text_delta.data_len, evt->data.text_delta.data);
        break;
    case OTOOL_LLM_TEXT_EVENT_COMPLETED:
        printf("\n[completed]\n");
        break;
    case OTOOL_LLM_TEXT_EVENT_ERROR:
        printf("\n[error] %s\n", evt->data.error.message);
        break;
    default:
        break;
    }
    return OTOOL_LLM_EVENT_ACTION_CONTINUE;
}

void llm_example(void)
{
    /* Runtime credential: never compile an api key into the firmware. */
    otool_llm_client_config_t cfg = {
        .struct_size = sizeof(cfg),
        .provider = OTOOL_LLM_PROVIDER_VOLCENGINE_ARK,
        .protocol = OTOOL_LLM_PROTOCOL_AUTO,
        .api_key = get_key_from_nvs(),   /* e.g. "ark-..." */
    };
    otool_llm_client_handle_t client = NULL;
    if (otool_llm_client_create(&cfg, &client) != ESP_OK) {
        return;
    }

    otool_llm_text_message_t msg = { .role = OTOOL_LLM_ROLE_USER, .text = "你好" };
    otool_llm_text_request_t req = {
        .struct_size = sizeof(req),
        .model = "doubao-seed-1-6-250615",
        .messages = &msg,
        .message_count = 1,
    };
    otool_llm_request_handle_t request = NULL;
    if (otool_llm_request_create(client, &req, &request) != ESP_OK) {
        otool_llm_client_destroy(client);
        return;
    }

    /* Blocking: run in a worker task, never on the LVGL/UI task. */
    esp_err_t err = otool_llm_request_execute_stream(request, on_event, NULL);

    otool_llm_request_destroy(request);
    otool_llm_client_destroy(client);
}
```

### Threading contract

- `otool_llm_request_execute_stream()` blocks and runs the callback in the
  calling task. Put it on a worker task; copy deltas and post them to a UI
  queue (do not call LVGL from the callback).
- `otool_llm_request_cancel()` is idempotent and safe from any task.
  Returning `OTOOL_LLM_EVENT_ACTION_CANCEL` from the callback is equivalent.
- `otool_llm_request_destroy()` is only valid after `execute_stream()` returns.
- One in-flight request per client; concurrent execution returns
  `ESP_ERR_INVALID_STATE`.
- Exactly one terminal event per request: `COMPLETED`, `INCOMPLETE`,
  `CANCELLED` or `ERROR`.

### Security

- The API key is deep-copied at `client_create()` and secure-zeroed at destroy.
- HTTPS is enforced by default (`OTOOL_LLM_ALLOW_INSECURE_HTTP` is for local
  testing only); the certificate bundle is always attached.
- No Authorization header, api key, full request body or user content is ever
  logged.

## Memory budgets (Kconfig)

| Option | Default | Meaning |
| --- | ---: | --- |
| `OTOOL_LLM_MAX_REQUEST_BYTES` | 32768 | Serialized request body cap |
| `OTOOL_LLM_MAX_SSE_EVENT_BYTES` | 16384 | One merged SSE event cap |
| `OTOOL_LLM_MAX_ERROR_BODY_BYTES` | 4096 | Non-2xx error body cap |
| `OTOOL_LLM_HTTP_RX_BUFFER_BYTES` | 2048 | HTTP receive chunk buffer |
| `OTOOL_LLM_CONNECT_TIMEOUT_MS` | 15000 | Connect timeout |
| `OTOOL_LLM_READ_TIMEOUT_MS` | 60000 | Stream read timeout |

All caps are hard errors, never silent truncation.
