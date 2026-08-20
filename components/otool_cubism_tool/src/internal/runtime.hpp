/**
 * @file runtime.hpp
 * @brief otool_cubism_tool — runtime 内部接口（仅组件私有源码包含）
 */

#pragma once

#include "otool_cubism_types.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace otool::cubism::runtime {

/** runtime 任务参数 */
struct coordinator_args {
    otool_cubism_metrics_t *metrics;      /*!< 由 facade 持有并加锁 */
    int64_t *started_at_us;               /*!< RUNNING 起始单调时间 */
    uint32_t frame_interval_us;
};

/**
 * @brief 运行协调任务（S0：指标采样与心跳，不产帧）
 *
 * 循环采样 heap/PSRAM 指标并维持 uptime；
 * 收到任务通知后退出（stop() 路径）。
 */
void coordinator_task(void *arg);

} // namespace otool::cubism::runtime
