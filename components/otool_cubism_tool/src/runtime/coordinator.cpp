/**
 * @file coordinator.cpp
 * @brief otool_cubism_tool — runtime 协调任务
 *
 * S0 阶段职责：以帧间隔节拍采样指标、维持 uptime，不产帧。
 * S1/S2 起在此任务内加入 presenter/player 驱动（见可行性报告 §4.10）。
 */

#include "internal/runtime.hpp"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"

namespace otool::cubism::runtime {
namespace {

constexpr char TAG[] = "cubism_rt";

} // namespace

void coordinator_task(void *arg)
{
    auto *args = static_cast<coordinator_args *>(arg);
    const TickType_t period_ticks =
        pdMS_TO_TICKS(args->frame_interval_us / 1000);

    while (1) {
        // 退出通知：stop() 路径
        if (ulTaskNotifyTake(pdTRUE, period_ticks) != 0) {
            break;
        }

#if CONFIG_OTOOL_CUBISM_METRICS
        args->metrics->heap_internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        args->metrics->heap_psram_free    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        args->metrics->largest_psram_block =
            heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
#endif
    }

    vTaskDelete(nullptr);
}

} // namespace otool::cubism::runtime
