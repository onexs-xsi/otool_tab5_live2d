idf_component_register(
    SRCS
        "src/agent/tool_registry.c"
        "src/agent/tool_schema.c"
        "src/core/client.c"
        "src/core/error.c"
        "src/core/request.c"
        "src/core/secure_zero.c"
        "src/providers/provider_ark.c"
        "src/providers/provider_custom.c"
        "src/providers/provider_openai.c"
        "src/providers/provider_table.c"
        "src/protocols/chat_completions_sse.c"
        "src/protocols/protocol_resolve.c"
        "src/protocols/responses_sse.c"
        "src/transports/http_stream.c"
        "src/transports/sse_parser.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES esp_http_client
    PRIV_REQUIRES esp-tls espressif__cjson
)
