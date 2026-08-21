# otool_llm_sdk

ESP-IDF component for streaming Large Language Model (LLM) chat completions.
It uses the OpenAI-compatible chat completions API with Server-Sent Events (SSE),
so it can be used with Doubao / Volcano Ark or other compatible endpoints.

## Features

- Streaming `delta` content via callback
- Configurable model, system prompt, max tokens, temperature
- API key passed at runtime
- Endpoint and timeout configurable through Kconfig

## Usage

Add this component to `main/CMakeLists.txt` `REQUIRES`:

```cmake
REQUIRES
    otool_llm_sdk
```

Example:

```c
#include "otool_llm_sdk.h"

static void on_llm_event(const otool_llm_event_t *event, void *user_ctx)
{
    switch (event->type) {
    case OTOOL_LLM_EVENT_DELTA:
        printf("%.*s", (int)event->data_len, event->data);
        break;
    case OTOOL_LLM_EVENT_DONE:
        printf("\n[DONE]\n");
        break;
    case OTOOL_LLM_EVENT_ERROR:
        printf("\n[ERROR] %.*s\n", (int)event->data_len, event->data);
        break;
    default:
        break;
    }
}

void llm_example(void)
{
    otool_llm_chat_request_t req = {
        .api_key = DOUBAO_API_KEY,
        .model = "doubao-seed-1-6-250615",
        .user_message = "Hello, please introduce yourself.",
        .on_event = on_llm_event,
    };
    otool_llm_chat_stream(&req);
}
```

> Note: `otool_llm_chat_stream()` is blocking. Run it in a dedicated task when
> the UI must stay responsive.
