/**
 * @file esp_clock.cpp
 * @brief otool_cubism_tool — ESP-IDF 平台实现：默认单调时钟
 */

#include "internal/platform.hpp"

#include "esp_timer.h"

namespace otool::cubism::platform {
namespace {

int64_t esp_now_us(void *ctx)
{
    (void)ctx;
    return (int64_t)esp_timer_get_time();
}

const otool_cubism_clock_port_t s_clock_port = {
    .ctx = nullptr,
    .now_us = esp_now_us,
};

} // namespace

const otool_cubism_clock_port_t *default_clock_port()
{
    return &s_clock_port;
}

} // namespace otool::cubism::platform
