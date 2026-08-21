idf_component_register(
    SRCS "main.cpp" "llm_app.cpp" "wifi_app.cpp" "ui_app.cpp" "console_cmds.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        otool_dev_autodetect
        otool_tab5_component
        otool_lvgl_idf_port
        otool_llm_sdk
        espressif__esp_hosted
        espressif__esp_wifi_remote
        lvgl__lvgl
        nvs_flash
        esp_netif
        esp_wifi
        esp_event
        console
)

# API Key 不再编译进固件（WP0 安全清理，见 docs/otool_llm_sdk_streaming_voice_implementation_plan.md §8.3）。
# 运行时由应用通过 otool_llm_client_config_t.api_key 或 Kconfig 注入。