# otool_cubism_tool — Live2D/Cubism 领域组件（可行性报告 §4）
#
# S0 阶段：clip-only 骨架。
#   - CLIP_PLAYER 默认开启（产帧逻辑 S2 实现）
#   - STREAM_CLIENT / REALTIME 的 Kconfig 默认关闭
#   - REALTIME 尚未实现：开启即 FATAL_ERROR（Core Gate 通过前不允许）
#   - 组件私有 C++ 统一 -fno-exceptions -fno-rtti，不改动工程整体语义

set(otool_cubism_tool_srcs
    "src/otool_cubism_tool.cpp"
    "src/runtime/coordinator.cpp"
    "src/platform/esp_clock.cpp"
)

if(CONFIG_OTOOL_CUBISM_ENABLE_STREAM_CLIENT)
    # S2b 起追加：src/stream/*.cpp
    message(STATUS "otool_cubism_tool: STREAM_CLIENT enabled (implementation pending S2b)")
endif()

if(CONFIG_OTOOL_CUBISM_ENABLE_REALTIME)
    message(FATAL_ERROR
        "otool_cubism_tool: REALTIME is not implemented yet. "
        "It stays disabled until the Core Gate (feasibility report §6.3/§10.1 S3-S5) passes.")
endif()

# REALTIME 必须配唯一 Core backend（§4.6）；S0 阶段 REALTIME 已被上面拦截，
# 但保留检查以在 Gate 通过后直接生效。
if(CONFIG_OTOOL_CUBISM_CORE_BACKEND_NONE AND CONFIG_OTOOL_CUBISM_ENABLE_REALTIME)
    message(FATAL_ERROR "otool_cubism_tool: REALTIME requires a Core backend (NONE forbidden)")
endif()
if(CONFIG_OTOOL_CUBISM_CORE_BACKEND_OFFICIAL AND NOT CONFIG_OTOOL_CUBISM_ENABLE_REALTIME)
    message(STATUS "otool_cubism_tool: OFFICIAL backend selected but REALTIME disabled; ignored")
endif()
if(CONFIG_OTOOL_CUBISM_CORE_BACKEND_PURISM AND NOT CONFIG_OTOOL_CUBISM_ENABLE_REALTIME)
    message(STATUS "otool_cubism_tool: PURISM backend selected but REALTIME disabled; ignored")
endif()

idf_component_register(
    SRCS ${otool_cubism_tool_srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES
    PRIV_REQUIRES
        esp_timer
        freertos
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -fno-exceptions
    -fno-rtti
)

# host tools（clip_packer / core_probe / asset_validator）不进入固件（§4.1）
