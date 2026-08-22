idf_component_register(
    SRCS
        "src/gzip_codec.c"
        "src/pcm_utils.c"
        "src/speech_memory.c"
        "src/volcengine_asr.c"
        "src/volcengine_asr_file.c"
        "src/volcengine_tts.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES esp_http_client espressif__esp_websocket_client
    PRIV_REQUIRES esp-tls esp_rom mbedtls espressif__cjson
)
