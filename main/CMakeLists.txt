idf_component_register(
    SRCS "credential_store.cpp" "main.cpp" "llm_app.cpp" "wifi_app.cpp" "ui_app.cpp" "console_cmds.cpp" "agent_app.cpp"
    INCLUDE_DIRS "."
    EMBED_FILES
        "fonts/noto_cn_fonts/noto_zh_mid_24.bin"
        "assets/agent_state_idle_argb8888.bin"
        "assets/agent_state_listening_argb8888.bin"
        "assets/agent_state_thinking_argb8888.bin"
        "assets/agent_state_tool_argb8888.bin"
        "assets/agent_state_speaking_argb8888.bin"
        "assets/agent_state_success_argb8888.bin"
        "assets/agent_state_error_argb8888.bin"
    REQUIRES
        otool_dev_autodetect
        otool_tab5_component
        otool_lvgl_idf_port
        otool_llm_sdk
        otool_speech_sdk
        espressif__esp_hosted
        espressif__esp_wifi_remote
        lvgl__lvgl
        nvs_flash
        esp_netif
        esp_wifi
        esp_event
        console
        espressif__cjson
)

# 部署凭证由本地 sdkconfig/menuconfig 提供（仓库默认均为空，sdkconfig 已忽略）。
# credential_store 仍保留 NVS 作为对应 sdkconfig 项为空时的可选后备。
