/**
 * @file otool_cubism_types.h
 * @brief otool_cubism_tool — 公共类型定义（配置、事件、状态、指标）
 *
 * 本头文件只暴露 esp_err_t 与自有 POD 类型，不暴露任何
 * Cubism Core / Framework / LVGL / PPA / 第三方容器类型。
 */

#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 运行模式（对应可行性报告路线 A/B/C/D）                                */
/* ------------------------------------------------------------------ */

typedef enum {
    OTOOL_CUBISM_MODE_CLIP_PLAYER = 0, /*!< 路线 D：预渲染片段播放（默认） */
    OTOOL_CUBISM_MODE_STREAM_CLIENT,   /*!< 路线 C：主机实时渲染串流接收 */
    OTOOL_CUBISM_MODE_REALTIME,        /*!< 路线 A/B：板端实时渲染 */
} otool_cubism_mode_t;

/* ------------------------------------------------------------------ */
/* 生命周期状态（状态迁移表见 otool_cubism_tool.h / 可行性报告 §4.4）     */
/* ------------------------------------------------------------------ */

typedef enum {
    OTOOL_CUBISM_STATE_UNINITIALIZED = 0,
    OTOOL_CUBISM_STATE_READY,      /*!< init() 完成，未加载素材 */
    OTOOL_CUBISM_STATE_LOADED,     /*!< load_package() 成功 */
    OTOOL_CUBISM_STATE_RUNNING,    /*!< start() 成功，正在产帧 */
    OTOOL_CUBISM_STATE_CLIP_FALLBACK, /*!< 实时路径故障，播放 Flash fallback */
    OTOOL_CUBISM_STATE_ERROR,      /*!< 不可恢复故障，停止产帧 */
} otool_cubism_state_t;

/* ------------------------------------------------------------------ */
/* 输入事件                                                             */
/* ------------------------------------------------------------------ */

typedef enum {
    OTOOL_CUBISM_INPUT_NONE = 0,
    OTOOL_CUBISM_INPUT_TAP,        /*!< 离散点击（按 sequence 去重） */
    OTOOL_CUBISM_INPUT_DRAG,       /*!< 连续坐标（队列满时合并保留最新） */
    OTOOL_CUBISM_INPUT_AUDIO_LEVEL,/*!< 音频幅度 0..255 */
    OTOOL_CUBISM_INPUT_COMMAND,    /*!< 离散命令（按 sequence 去重） */
} otool_cubism_input_kind_t;

typedef struct {
    otool_cubism_input_kind_t kind;
    uint32_t sequence;          /*!< 离散事件去重序号 */
    int64_t timestamp_us;       /*!< 单调时钟时间戳（组件丢弃过期坐标） */
    int16_t x;                  /*!< 坐标，归一化 -32768..32767 */
    int16_t y;
    uint8_t level;              /*!< audio 幅度 0..255 */
    uint8_t flags;              /*!< 保留 */
} otool_cubism_input_event_t;

/* ------------------------------------------------------------------ */
/* 实时参数更新（仅 REALTIME 模式有效）                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t parameter_id_hash; /*!< 由资产 manifest 提供的参数 ID 哈希 */
    float value;
} otool_cubism_parameter_update_t;

/* ------------------------------------------------------------------ */
/* 状态与指标查询                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    otool_cubism_state_t state;
    otool_cubism_mode_t active_mode;
    uint32_t package_version;   /*!< 已加载素材包版本，0 = 未加载 */
    esp_err_t last_error;       /*!< 最近一次错误（ESP_OK 表示无） */
    uint32_t uptime_ms;         /*!< 组件运行时长（RUNNING 起算） */
} otool_cubism_status_t;

typedef struct {
    uint32_t frames_rendered;
    uint32_t frames_dropped;
    uint32_t input_dropped;         /*!< 输入队列满丢弃的非 DRAG 事件数 */
    uint32_t errors;
    uint32_t jpeg_decoded;
    uint32_t heap_internal_free;    /*!< 内部 RAM 剩余 */
    uint32_t heap_psram_free;       /*!< PSRAM 剩余 */
    uint32_t largest_psram_block;   /*!< PSRAM 最大连续块 */
    uint32_t last_frame_time_us;
    uint32_t frame_time_p95_us;     /*!< 滚动 P95 帧时间 */
} otool_cubism_metrics_t;

/* ------------------------------------------------------------------ */
/* 配置（端口为 C 函数表指针，见 otool_cubism_port.h）                    */
/* ------------------------------------------------------------------ */

typedef struct otool_cubism_display_port otool_cubism_display_port_t;
typedef struct otool_cubism_storage_port otool_cubism_storage_port_t;
typedef struct otool_cubism_stream_port otool_cubism_stream_port_t;
typedef struct otool_cubism_clock_port otool_cubism_clock_port_t;

typedef struct {
    const otool_cubism_display_port_t *display; /*!< 必填；start() 前必须可获取租约 */
    const otool_cubism_storage_port_t *storage; /*!< 可选；素材在 SD/Flash 时必填 */
    const otool_cubism_stream_port_t *stream;   /*!< 可选；仅 STREAM_CLIENT 需要 */
    const otool_cubism_clock_port_t *clock;     /*!< 可选；NULL 则用 esp_timer_get_time() */

    uint32_t frame_interval_us;   /*!< 目标帧间隔，默认 33333（30 FPS） */
    uint32_t input_queue_depth;   /*!< 输入队列深度，默认 8 */
    uint32_t metrics_enabled;     /*!< 是否采集指标（Kconfig 可覆盖） */
} otool_cubism_config_t;

#ifdef __cplusplus
} /* extern "C" */
#endif
