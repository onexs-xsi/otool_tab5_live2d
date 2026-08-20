# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(otool_tab5_live2d)

# 把 main/ 加入 LVGL 的 include 路径，使 lv_assert_override.h 可被找到
# （与 tab5_breath_control_tool 相同的处理方式）
target_include_directories(__idf_lvgl__lvgl PUBLIC ${CMAKE_SOURCE_DIR}/main)
