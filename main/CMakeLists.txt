idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        otool_dev_autodetect
        otool_tab5_component
        otool_lvgl_idf_port
        otool_llm_sdk
        lvgl__lvgl
        nvs_flash
)

# 豆包大模型 API Key
# 注意：此密钥仅用于本地开发，请勿提交到公开仓库。
set(DOUBAO_API_KEY "OTOOL_LLM_API_KEY_FROM_SDKCONFIG")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    DOUBAO_API_KEY="\"${DOUBAO_API_KEY}\""
)
