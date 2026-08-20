# otool_cubism_tool — Live2D/Cubism 领域组件（可行性报告 §4）
#
# 主路线：组件内自研 moc3/Core 兼容运行时（SELF_CORE，路线 E）。
# 当前阶段：S0 骨架 + 研发治理骨架（spec/research/test 均为 draft）。
#
# 材料与构建规则（2026-08-21）：
#   - 允许联网拉取任何研究材料、允许逆向任何文件；材料一律放在临时目录，
#     不得进入仓库/组件目录；组件内实现代码仍全部自研。
#   - REALTIME 构建门禁已移除：REALTIME=y 可编译（当前为骨架，功能实现中，
#     未实现部分以 WARNING 提示，不再 FATAL_ERROR）。
#   - clip-only 构建不得出现 Core/Framework 符号。
#   - 组件私有 C++ 统一 -fno-exceptions -fno-rtti。

set(otool_cubism_tool_srcs
    "src/otool_cubism_tool.cpp"
    "src/runtime/coordinator.cpp"
    "src/platform/esp_clock.cpp"
)

if(CONFIG_OTOOL_CUBISM_ENABLE_STREAM_CLIENT)
    # S2b 起追加：src/stream/*.cpp
    message(STATUS "otool_cubism_tool: STREAM_CLIENT enabled (implementation pending S2b)")
endif()

# ---- REALTIME 提示（门禁已移除，2026-08-21）----
if(CONFIG_OTOOL_CUBISM_ENABLE_REALTIME)
    if(NOT CONFIG_OTOOL_CUBISM_CORE_BACKEND_SELF)
        message(WARNING
            "otool_cubism_tool: REALTIME without SELF backend — NONE backend "
            "provides clip/stream only; core sources will not be built")
    endif()
    if(NOT CONFIG_OTOOL_CUBISM_MOC_PROFILE_V5)
        message(WARNING
            "otool_cubism_tool: REALTIME without CONFIG_OTOOL_CUBISM_MOC_PROFILE_V5 "
            "(only releasable profile for v1)")
    endif()
    if(CONFIG_OTOOL_CUBISM_ENABLE_V6_OFFSCREEN)
        message(WARNING
            "otool_cubism_tool: v6 offscreen is not implemented yet; "
            "the option currently has no effect")
    endif()

    if(CONFIG_OTOOL_CUBISM_CORE_BACKEND_SELF)
        # self Core（C0 起，逐层追加；全部自研，见 docs/research_log.md）
        list(APPEND otool_cubism_tool_srcs
            "src/core/self/moc3_validate.cpp"
        )
    endif()
    message(STATUS "otool_cubism_tool: REALTIME (SELF) enabled")
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

# host tools / research / spec / test vectors 不进入固件（§4.1、§10.2）
