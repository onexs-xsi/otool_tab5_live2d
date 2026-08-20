/**
 * @file otool_cubism_tool.cpp
 * @brief otool_cubism_tool — 生命周期门面与状态机
 *
 * S0 实现范围（可行性报告 §10.1）：
 *   - 公共 API + 端口校验
 *   - 状态机：UNINITIALIZED/READY/LOADED/RUNNING/CLIP_FALLBACK/ERROR
 *   - load_package：S0 最小校验（文件存在且非空），manifest 解析 S2 完善
 *   - start：display 租约（acquire）→ runtime 协调任务 → RUNNING
 *   - stop：任务停止并等待 → 归还租约 → LOADED
 *   - 输入：DRAG 保留最新状态；其他事件走有界队列
 *   - REALTIME / STREAM_CLIENT 模式在 S0 返回 ESP_ERR_NOT_SUPPORTED
 */

#include "otool_cubism_tool.h"

#include "internal/platform.hpp"
#include "internal/runtime.hpp"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <cstring>
#include <new>

namespace otool::cubism {
namespace {

constexpr char TAG[] = "cubism";

constexpr uint32_t DEFAULT_FRAME_INTERVAL_US = 33333; /* 30 FPS */
constexpr uint32_t DEFAULT_INPUT_QUEUE_DEPTH = 8;
constexpr uint32_t STOP_TASK_TIMEOUT_MS = 2000;

bool is_active_state(otool_cubism_state_t s)
{
    return s == OTOOL_CUBISM_STATE_RUNNING || s == OTOOL_CUBISM_STATE_CLIP_FALLBACK;
}

} // namespace

/** 组件内部状态：由 lock 互斥保护（state/active_mode/metrics/latest_drag） */
struct otool_cubism_tool::impl {
    otool_cubism_config_t cfg{};

    SemaphoreHandle_t lock = nullptr;
    QueueHandle_t input_queue = nullptr;

    otool_cubism_state_t state = OTOOL_CUBISM_STATE_UNINITIALIZED;
    otool_cubism_mode_t active_mode = OTOOL_CUBISM_MODE_CLIP_PLAYER;
    bool display_leased = false;
    uint32_t package_version = 0;
    esp_err_t last_error = ESP_OK;

    TaskHandle_t coordinator_task = nullptr;
    runtime::coordinator_args coordinator_args{};

    int64_t started_at_us = 0;

    otool_cubism_input_event_t latest_drag{};
    bool has_latest_drag = false;

    otool_cubism_metrics_t metrics{};
};

otool_cubism_tool::otool_cubism_tool() : impl_(new (std::nothrow) impl) {}

otool_cubism_tool::~otool_cubism_tool()
{
    deinit();
    delete impl_;
    impl_ = nullptr;
}

esp_err_t otool_cubism_tool::init(const otool_cubism_config_t &cfg)
{
    if (impl_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    if (impl_->state != OTOOL_CUBISM_STATE_UNINITIALIZED) {
        return ESP_ERR_INVALID_STATE;
    }

    // 端口校验（可行性报告 §4.5）：display 端口对象必须存在；
    // 租约可用性在 start() 时才判定。
    if (cfg.display == nullptr) {
        ESP_LOGE(TAG, "init: display port is required");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg.display->acquire == nullptr || cfg.display->release == nullptr ||
        cfg.display->get_info == nullptr) {
        ESP_LOGE(TAG, "init: display port functions incomplete");
        return ESP_ERR_INVALID_ARG;
    }

    impl_->lock = xSemaphoreCreateMutex();
    if (impl_->lock == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    const uint32_t queue_depth =
        cfg.input_queue_depth != 0 ? cfg.input_queue_depth : DEFAULT_INPUT_QUEUE_DEPTH;
    impl_->input_queue = xQueueCreate(queue_depth, sizeof(otool_cubism_input_event_t));
    if (impl_->input_queue == nullptr) {
        vSemaphoreDelete(impl_->lock);
        impl_->lock = nullptr;
        return ESP_ERR_NO_MEM;
    }

    impl_->cfg = cfg;
    impl_->cfg.frame_interval_us =
        cfg.frame_interval_us != 0 ? cfg.frame_interval_us : DEFAULT_FRAME_INTERVAL_US;
    impl_->cfg.input_queue_depth = queue_depth;
    impl_->cfg.clock = cfg.clock != nullptr ? cfg.clock : platform::default_clock_port();

    impl_->state = OTOOL_CUBISM_STATE_READY;
    impl_->last_error = ESP_OK;
    ESP_LOGI(TAG, "init: READY (frame_interval=%luus queue_depth=%lu)",
             (unsigned long)impl_->cfg.frame_interval_us,
             (unsigned long)queue_depth);
    return ESP_OK;
}

esp_err_t otool_cubism_tool::load_package(const char *manifest_path)
{
    if (impl_ == nullptr || impl_->lock == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (manifest_path == nullptr || manifest_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (impl_->state != OTOOL_CUBISM_STATE_READY &&
        impl_->state != OTOOL_CUBISM_STATE_LOADED) {
        return ESP_ERR_INVALID_STATE;
    }
    if (impl_->cfg.storage == nullptr || impl_->cfg.storage->open == nullptr ||
        impl_->cfg.storage->close == nullptr) {
        ESP_LOGE(TAG, "load_package: storage port not available");
        return ESP_ERR_NOT_SUPPORTED;
    }

    // S0 最小校验：文件存在且非空；manifest 解析（magic/版本/CRC/profile）S2 实现。
    int32_t handle = -1;
    esp_err_t err = impl_->cfg.storage->open(impl_->cfg.storage->ctx, manifest_path, &handle);
    if (err != ESP_OK || handle < 0) {
        ESP_LOGE(TAG, "load_package: open '%s' failed: %s", manifest_path,
                 esp_err_to_name(err != ESP_OK ? err : ESP_ERR_NOT_FOUND));
        xSemaphoreTake(impl_->lock, portMAX_DELAY);
        impl_->last_error = err != ESP_OK ? err : ESP_ERR_NOT_FOUND;
        xSemaphoreGive(impl_->lock);
        return impl_->last_error;
    }

    uint64_t size = 0;
    err = impl_->cfg.storage->size(impl_->cfg.storage->ctx, handle, &size);
    if (err != ESP_OK || size == 0) {
        ESP_LOGE(TAG, "load_package: invalid size for '%s'", manifest_path);
        (void)impl_->cfg.storage->close(impl_->cfg.storage->ctx, handle);
        xSemaphoreTake(impl_->lock, portMAX_DELAY);
        impl_->last_error = err != ESP_OK ? err : ESP_ERR_INVALID_SIZE;
        xSemaphoreGive(impl_->lock);
        return impl_->last_error;
    }
    (void)impl_->cfg.storage->close(impl_->cfg.storage->ctx, handle);

    xSemaphoreTake(impl_->lock, portMAX_DELAY);
    impl_->package_version = 1; /* S0 占位版本；S2 起由 manifest 提供 */
    impl_->state = OTOOL_CUBISM_STATE_LOADED;
    impl_->last_error = ESP_OK;
    xSemaphoreGive(impl_->lock);

    ESP_LOGI(TAG, "load_package: LOADED '%s' (%llu bytes)", manifest_path,
             (unsigned long long)size);
    return ESP_OK;
}

esp_err_t otool_cubism_tool::start(otool_cubism_mode_t run_mode)
{
    if (impl_ == nullptr || impl_->lock == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(impl_->lock, portMAX_DELAY);
    if (impl_->state != OTOOL_CUBISM_STATE_LOADED) {
        xSemaphoreGive(impl_->lock);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(impl_->lock);

    // S0 模式门禁：REALTIME / STREAM_CLIENT 尚未实现（§10.1 S3/S2b）
    switch (run_mode) {
    case OTOOL_CUBISM_MODE_CLIP_PLAYER:
#if !CONFIG_OTOOL_CUBISM_ENABLE_CLIP_PLAYER
        return ESP_ERR_NOT_SUPPORTED;
#endif
        break;
    case OTOOL_CUBISM_MODE_STREAM_CLIENT:
        ESP_LOGW(TAG, "start: STREAM_CLIENT not implemented yet (S2b)");
        return ESP_ERR_NOT_SUPPORTED;
    case OTOOL_CUBISM_MODE_REALTIME:
        ESP_LOGW(TAG, "start: REALTIME not implemented yet (S3/S4/S5 Gate)");
        return ESP_ERR_NOT_SUPPORTED;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    // 显示独占租约：不可用时必须拒绝接管显示（§4.8、§12-3）
    esp_err_t err = impl_->cfg.display->acquire(impl_->cfg.display->ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start: display lease acquire failed: %s", esp_err_to_name(err));
        xSemaphoreTake(impl_->lock, portMAX_DELAY);
        impl_->last_error = err;
        xSemaphoreGive(impl_->lock);
        return err;
    }

    impl_->coordinator_args.metrics = &impl_->metrics;
    impl_->coordinator_args.started_at_us = &impl_->started_at_us;
    impl_->coordinator_args.frame_interval_us = impl_->cfg.frame_interval_us;

    BaseType_t ok = xTaskCreate(runtime::coordinator_task, "cubism_rt", 4096,
                                &impl_->coordinator_args, 5, &impl_->coordinator_task);
    if (ok != pdPASS) {
        (void)impl_->cfg.display->release(impl_->cfg.display->ctx);
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(impl_->lock, portMAX_DELAY);
    impl_->state = OTOOL_CUBISM_STATE_RUNNING;
    impl_->active_mode = run_mode;
    impl_->display_leased = true;
    impl_->started_at_us = impl_->cfg.clock->now_us(impl_->cfg.clock->ctx);
    impl_->last_error = ESP_OK;
    xSemaphoreGive(impl_->lock);

    ESP_LOGI(TAG, "start: RUNNING (mode=%d)", (int)run_mode);
    return ESP_OK;
}

esp_err_t otool_cubism_tool::submit_input(const otool_cubism_input_event_t &event)
{
    if (impl_ == nullptr || impl_->lock == nullptr || impl_->input_queue == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (event.kind == OTOOL_CUBISM_INPUT_NONE) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(impl_->lock, portMAX_DELAY);
    const bool active = is_active_state(impl_->state);
    xSemaphoreGive(impl_->lock);
    if (!active) {
        return ESP_ERR_INVALID_STATE;
    }

    if (event.kind == OTOOL_CUBISM_INPUT_DRAG) {
        // 连续坐标：保留最新状态（§4.4）
        xSemaphoreTake(impl_->lock, portMAX_DELAY);
        impl_->latest_drag = event;
        impl_->has_latest_drag = true;
        xSemaphoreGive(impl_->lock);
        return ESP_OK;
    }

    if (xQueueSend(impl_->input_queue, &event, 0) != pdTRUE) {
        xSemaphoreTake(impl_->lock, portMAX_DELAY);
        ++impl_->metrics.input_dropped;
        xSemaphoreGive(impl_->lock);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t otool_cubism_tool::set_parameter(const otool_cubism_parameter_update_t &update)
{
    (void)update;
    // 仅 REALTIME 模式有效（§4.4）；S0 尚未实现实时模式
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t otool_cubism_tool::get_status(otool_cubism_status_t *out) const
{
    if (impl_ == nullptr || impl_->lock == nullptr || out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(impl_->lock, portMAX_DELAY);
    out->state = impl_->state;
    out->active_mode = impl_->active_mode;
    out->package_version = impl_->package_version;
    out->last_error = impl_->last_error;
    out->uptime_ms = impl_->started_at_us != 0
                         ? (uint32_t)((impl_->cfg.clock->now_us(impl_->cfg.clock->ctx) -
                                       impl_->started_at_us) / 1000)
                         : 0;
    xSemaphoreGive(impl_->lock);
    return ESP_OK;
}

esp_err_t otool_cubism_tool::get_metrics(otool_cubism_metrics_t *out) const
{
    if (impl_ == nullptr || impl_->lock == nullptr || out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(impl_->lock, portMAX_DELAY);
    *out = impl_->metrics;
    xSemaphoreGive(impl_->lock);
    return ESP_OK;
}

esp_err_t otool_cubism_tool::stop()
{
    if (impl_ == nullptr || impl_->lock == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(impl_->lock, portMAX_DELAY);
    const bool active = is_active_state(impl_->state);
    const TaskHandle_t task = impl_->coordinator_task;
    xSemaphoreGive(impl_->lock);
    if (!active) {
        return ESP_ERR_INVALID_STATE;
    }

    // 1. 停止产帧：通知协调任务退出并等待其删除
    if (task != nullptr) {
        xTaskNotify(task, 1, eSetBits);
        // 等待任务自行 vTaskDelete；以有界超时兜底
        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(STOP_TASK_TIMEOUT_MS);
        while (eTaskGetState(task) != eDeleted && xTaskGetTickCount() < deadline) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    // 2. 归还显示租约（§4.4：确认静止后才归还）
    xSemaphoreTake(impl_->lock, portMAX_DELAY);
    const bool leased = impl_->display_leased;
    impl_->coordinator_task = nullptr;
    impl_->display_leased = false;
    xSemaphoreGive(impl_->lock);

    if (leased) {
        esp_err_t err = impl_->cfg.display->release(impl_->cfg.display->ctx);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "stop: display lease release failed: %s", esp_err_to_name(err));
            xSemaphoreTake(impl_->lock, portMAX_DELAY);
            impl_->state = OTOOL_CUBISM_STATE_ERROR;
            impl_->last_error = err;
            xSemaphoreGive(impl_->lock);
            return err;
        }
    }

    xSemaphoreTake(impl_->lock, portMAX_DELAY);
    impl_->state = OTOOL_CUBISM_STATE_LOADED;
    impl_->started_at_us = 0;
    impl_->has_latest_drag = false;
    impl_->last_error = ESP_OK;
    xSemaphoreGive(impl_->lock);

    ESP_LOGI(TAG, "stop: LOADED");
    return ESP_OK;
}

esp_err_t otool_cubism_tool::deinit()
{
    if (impl_ == nullptr) {
        return ESP_OK;
    }

    if (impl_->lock != nullptr) {
        xSemaphoreTake(impl_->lock, portMAX_DELAY);
        const bool active = is_active_state(impl_->state);
        xSemaphoreGive(impl_->lock);
        if (active) {
            ESP_LOGE(TAG, "deinit: must stop() before deinit()");
            return ESP_ERR_INVALID_STATE;
        }
    }

    if (impl_->input_queue != nullptr) {
        vQueueDelete(impl_->input_queue);
        impl_->input_queue = nullptr;
    }
    if (impl_->lock != nullptr) {
        vSemaphoreDelete(impl_->lock);
        impl_->lock = nullptr;
    }

    impl_->state = OTOOL_CUBISM_STATE_UNINITIALIZED;
    impl_->package_version = 0;
    impl_->started_at_us = 0;
    impl_->last_error = ESP_OK;
    std::memset(&impl_->metrics, 0, sizeof(impl_->metrics));
    ESP_LOGI(TAG, "deinit: UNINITIALIZED");
    return ESP_OK;
}

} // namespace otool::cubism
