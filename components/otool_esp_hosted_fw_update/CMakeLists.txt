set(OTOOL_ESP_HOSTED_FW_UPDATE_SRCS
    "src/core/otool_esp_hosted_fw_update_core.cpp"
    "src/core/otool_esp_hosted_fw_update_registry.cpp"
    "src/providers/otool_esp_hosted_fw_provider_stub.cpp"
    "src/ports/espidf/otool_esp_hosted_fw_platform_espidf.cpp"
    "src/ports/espidf/otool_esp_hosted_fw_transport_esp_hosted.cpp"
    "src/ports/espidf/otool_esp_hosted_fw_update_minimal_init.cpp"
)

if(CONFIG_OTOOL_ESP_HOSTED_FW_ENABLE_COMPAT_API)
    list(APPEND OTOOL_ESP_HOSTED_FW_UPDATE_SRCS "src/compat/otool_esp_hosted_wifi_firmware_ota_compat.cpp")
endif()

# console 命令支持（可选）
if(CONFIG_OTOOL_ESP_HOSTED_FW_ENABLE_CONSOLE_CMDS)
    list(APPEND OTOOL_ESP_HOSTED_FW_UPDATE_SRCS "src/console/otool_esp_hosted_fw_console.cpp")
endif()

# -------------------------------------------------------------------
# 自动扫描 firmware/*_wifi_firmware/ 下所有 .bin 并生成固件表 .cpp
# 支持多芯片目录（如将来增加 esp32c5_wifi_firmware）
# 提示：新增/删除 .bin 文件后需执行 idf.py reconfigure
# -------------------------------------------------------------------
set(OTOOL_ESP_HOSTED_FW_UPDATE_EMBED_FILES "")

if(CONFIG_OTOOL_ESP_HOSTED_FW_PROVIDER_EMBEDDED)
    set(_EFW_COPY_DIR "${CMAKE_CURRENT_BINARY_DIR}/efw")
    file(MAKE_DIRECTORY "${_EFW_COPY_DIR}")

    # CONFIGURE_DEPENDS：.bin 文件增减时自动触发 CMake 重配置
    file(GLOB_RECURSE _ALL_FW_BINS
        CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/firmware/*_wifi_firmware/*.bin")

    set(_GEN_EXTERNS "")
    set(_GEN_TABLE_ENTRIES "")
    set(_USED_LOGICAL_NAMES "")

    foreach(_BIN_FILE ${_ALL_FW_BINS})
        # 从父目录名提取芯片短名，e.g. esp32c6_wifi_firmware → c6
        get_filename_component(_BIN_DIR       "${_BIN_FILE}" DIRECTORY)
        get_filename_component(_CHIP_DIR_NAME "${_BIN_DIR}"  NAME)
        string(REGEX REPLACE "^esp32([a-z0-9]+)_wifi_firmware$"
               "\\1" _CHIP_SHORT "${_CHIP_DIR_NAME}")

        get_filename_component(_BIN_NAME "${_BIN_FILE}" NAME)    # fw_bluedroid.bin
        string(REGEX REPLACE "\\.bin$" "" _BIN_STEM "${_BIN_NAME}")
        # 剥去可选的 fw_ 前缀得到基础名称
        string(REGEX REPLACE "^fw_" "" _BASE_NAME "${_BIN_STEM}") # bluedroid
        # 逻辑名保留完整变体信息，同时把 '.' / '-' 等替换成 '_'，
        # 避免 ESP-IDF 在 EMBED_FILES 阶段把多个版本名压成同一个符号。
        string(REGEX REPLACE "[^A-Za-z0-9]+" "_" _SAFE_BASE_NAME "${_BASE_NAME}")
        string(REGEX REPLACE "_+" "_" _SAFE_BASE_NAME "${_SAFE_BASE_NAME}")
        string(REGEX REPLACE "^_|_$" "" _SAFE_BASE_NAME "${_SAFE_BASE_NAME}")
        # 逻辑名：c6_bluedroid / c6_c6wifi_1_12_0_nimble
        set(_LOGICAL_NAME "${_CHIP_SHORT}_${_SAFE_BASE_NAME}")

        if(_LOGICAL_NAME IN_LIST _USED_LOGICAL_NAMES)
            message(FATAL_ERROR
                "[otool_esp_hosted_fw_update] duplicated firmware logical name '${_LOGICAL_NAME}'\n"
                "  first/other source includes: ${_BIN_NAME}\n"
                "  please rename the conflicting .bin files so their sanitized stems are unique.")
        endif()
        list(APPEND _USED_LOGICAL_NAMES "${_LOGICAL_NAME}")

        # 复制为安全且唯一的 basename，防止符号名冲突
        set(_COPY_NAME "${_LOGICAL_NAME}.bin")                   # c6_c6wifi_1_12_0_nimble.bin
        set(_COPY_PATH "${_EFW_COPY_DIR}/${_COPY_NAME}")
        configure_file("${_BIN_FILE}" "${_COPY_PATH}" COPYONLY)

        list(APPEND OTOOL_ESP_HOSTED_FW_UPDATE_EMBED_FILES "${_COPY_PATH}")

        # C 符号由 basename 推导：c6_bluedroid.bin → c6_bluedroid_bin
        string(REPLACE "." "_" _SYM "${_COPY_NAME}")

        string(APPEND _GEN_EXTERNS
            "extern const uint8_t _binary_${_SYM}_start[];\n"
            "extern const uint8_t _binary_${_SYM}_end[];\n")
        string(APPEND _GEN_TABLE_ENTRIES
            "    {\"${_LOGICAL_NAME}\", \"${_BIN_NAME}\","
            " _binary_${_SYM}_start, _binary_${_SYM}_end},\n")
    endforeach()

    if(NOT _ALL_FW_BINS)
        message(WARNING
            "[otool_esp_hosted_fw_update] firmware/*_wifi_firmware/ 下未发现 .bin 文件，"
            "embedded provider 固件表为空。请确认目录结构或执行 idf.py reconfigure。")
    endif()

    # 用模板生成 embedded provider 完整实现（替代手工维护的 embedded.cpp）
    set(_GEN_FILE "${CMAKE_CURRENT_BINARY_DIR}/otool_esp_hosted_fw_gen.cpp")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/otool_esp_hosted_fw_gen.cpp.in"
        "${_GEN_FILE}" @ONLY)

    list(APPEND OTOOL_ESP_HOSTED_FW_UPDATE_SRCS "${_GEN_FILE}")
endif()

idf_component_register(
    SRCS ${OTOOL_ESP_HOSTED_FW_UPDATE_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src" "src/core" "src/providers" "src/ports/espidf" "src/console"
    PRIV_REQUIRES esp_hosted nvs_flash esp_event esp_netif esp_wifi console
    EMBED_FILES ${OTOOL_ESP_HOSTED_FW_UPDATE_EMBED_FILES}
)

# esp_wifi_default_config.h 的 CONFIG_WIFI_RMT_* 宏依赖 CONFIG_SLAVE_IDF_TARGET_*
# 当 sdkconfig 未启用 esp_wifi_remote（IS_DISABLED=y）时这些宏不会被生成，
# 需要手动指定协处理器目标为 ESP32-C6。
target_compile_definitions(__idf_otool_esp_hosted_fw_update PRIVATE
    CONFIG_SLAVE_IDF_TARGET_ESP32C6=1
)
