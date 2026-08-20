idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        otool_dev_autodetect
        otool_tab5_component
        otool_lvgl_idf_port
        otool_cubism_tool
        lvgl__lvgl
        nvs_flash
)

# 演示素材：第三方研究材料，仅构建期从临时目录嵌入，不进入仓库。
# 可用环境变量 OTOOL_MAO_DIR 覆盖素材目录（默认 %TEMP%/otool_cubism_research）。
set(OTOOL_MAO_DIR "$ENV{OTOOL_MAO_DIR}")
if(NOT OTOOL_MAO_DIR)
    set(OTOOL_MAO_DIR "$ENV{TEMP}/otool_cubism_research")
endif()

if(NOT EXISTS "${OTOOL_MAO_DIR}/Mocari/assets/models/Mao/Mao.moc3")
    message(FATAL_ERROR "Mao model material not found under ${OTOOL_MAO_DIR} (set OTOOL_MAO_DIR)")
endif()

target_add_binary_data(${COMPONENT_LIB}
    "${OTOOL_MAO_DIR}/Mocari/assets/models/Mao/Mao.moc3" mao_moc3)
target_add_binary_data(${COMPONENT_LIB}
    "${OTOOL_MAO_DIR}/mao_tex.raw" mao_tex)
