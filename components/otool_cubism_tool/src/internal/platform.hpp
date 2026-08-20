/**
 * @file platform.hpp
 * @brief otool_cubism_tool — 平台内部接口（仅组件私有源码包含）
 */

#pragma once

#include "otool_cubism_port.h"

namespace otool::cubism::platform {

/** 默认时钟端口：esp_timer_get_time()（单调微秒） */
const otool_cubism_clock_port_t *default_clock_port();

} // namespace otool::cubism::platform
