/**
 * @file otool_cubism_tool.h
 * @brief otool_cubism_tool — 唯一业务门面
 *
 * 公共 API 小而稳定：生命周期 + 输入 + 参数 + 查询。
 * main 只负责创建板级对象、注入端口并选择运行模式，
 * 不直接调用 Cubism Core / Framework / JPEG / PPA / 播放器内部接口。
 *
 * 状态迁移（可行性报告 §4.4）：
 *
 *   UNINITIALIZED --init--> READY
 *   READY/LOADED   --load_package--> LOADED      （失败保持原状态/旧包）
 *   LOADED         --start--> RUNNING            （先取得显示租约）
 *   RUNNING/CLIP_FALLBACK --stop--> LOADED       （确认产帧/DMA 静止后归还租约）
 *   READY/LOADED   --deinit--> UNINITIALIZED     （幂等）
 *   RUNNING        --可恢复故障--> CLIP_FALLBACK （仅 fallback 校验过时）
 *   任意活动状态   --不可恢复故障--> ERROR
 *   ERROR          --stop/deinit--> READY | UNINITIALIZED
 *
 * deinit() 只能从非 RUNNING 状态执行；RUNNING 必须先 stop()。
 * 模式切换必须经过 stop → load（如需要）→ start。
 */

#pragma once

#include "otool_cubism_types.h"

#ifdef __cplusplus

#include "esp_err.h"

namespace otool::cubism {

class otool_cubism_tool {
public:
    otool_cubism_tool();
    ~otool_cubism_tool();

    /*! 只分配基础资源并校验端口，不接管显示。
     *  返回 ESP_ERR_INVALID_ARG：display 端口缺失/配置非法。 */
    esp_err_t init(const otool_cubism_config_t &cfg);

    /*! 先验证 manifest、版本、CRC、模型 profile 与预算，再提交新资源；
     *  失败时旧资源保持可用。S0 阶段仅执行文件存在性与最小头部校验。 */
    esp_err_t load_package(const char *manifest_path);

    /*! 取得显示独占租约后创建工作任务。
     *  重复调用返回 ESP_ERR_INVALID_STATE。 */
    esp_err_t start(otool_cubism_mode_t run_mode);

    /*! 所有模式统一的非阻塞输入入口；队列满时合并连续坐标并保留最新状态。 */
    esp_err_t submit_input(const otool_cubism_input_event_t &event);

    /*! 仅 REALTIME 模式有效；其他模式返回 ESP_ERR_NOT_SUPPORTED。 */
    esp_err_t set_parameter(const otool_cubism_parameter_update_t &update);

    esp_err_t get_status(otool_cubism_status_t *out) const;
    esp_err_t get_metrics(otool_cubism_metrics_t *out) const;

    /*! 先停产帧，等待 PPA/JPEG 静止后归还显示租约。 */
    esp_err_t stop();

    /*! 幂等释放全部组件资源；仅允许从非 RUNNING 状态调用。 */
    esp_err_t deinit();

private:
    struct impl;
    impl *impl_;
};

} // namespace otool::cubism

#else /* __cplusplus */

#error "otool_cubism_tool requires C++"

#endif /* __cplusplus */
