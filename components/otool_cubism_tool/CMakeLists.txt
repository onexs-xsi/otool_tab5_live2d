# otool_cubism_tool — Live2D/Cubism 领域组件（可行性报告 §4）
#
# 主路线：组件内自研 moc3/Core 兼容运行时（SELF_CORE，路线 E）。
# 当前阶段：S0 骨架 + 研发治理骨架（spec/research/test 均为 draft）。
#
# 门禁规则：
#   - REALTIME 尚未实现：开启即 FATAL_ERROR，直到 generated/core_feature_manifest.h
#     由 spec/vector/corpus hash 生成并通过 G-FMT/G-BHV/G-SEC/G-TGT/G-RND/G-REL。
#   - REALTIME 必须配 SELF backend；NONE 仅允许 clip/stream。
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

# ---- REALTIME 门禁（§4.6 / §6.6）----
if(CONFIG_OTOOL_CUBISM_ENABLE_REALTIME)
    if(NOT CONFIG_OTOOL_CUBISM_CORE_BACKEND_SELF)
        message(FATAL_ERROR
            "otool_cubism_tool: REALTIME requires the SELF Core backend "
            "(NONE is clip/stream only)")
    endif()
    if(NOT CONFIG_OTOOL_CUBISM_MOC_PROFILE_V5)
        message(FATAL_ERROR
            "otool_cubism_tool: REALTIME requires CONFIG_OTOOL_CUBISM_MOC_PROFILE_V5 "
            "(only releasable profile for v1)")
    endif()
    if(CONFIG_OTOOL_CUBISM_ENABLE_V6_OFFSCREEN)
        message(FATAL_ERROR
            "otool_cubism_tool: v6 offscreen is a separate gated milestone; "
            "not available yet")
    endif()
    if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/generated/core_feature_manifest.h")
        message(FATAL_ERROR
            "otool_cubism_tool: REALTIME is locked until generated/"
            "core_feature_manifest.h exists (produced from approved spec/vector/"
            "corpus hashes, not by editing a macro)")
    endif()
    # Gate 打开后在此追加 core/self 与 renderer 源文件
    message(STATUS "otool_cubism_tool: REALTIME (SELF) sources pending")
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
