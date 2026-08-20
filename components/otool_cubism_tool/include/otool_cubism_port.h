/**
 * @file otool_cubism_port.h
 * @brief otool_cubism_tool — 端口注入接口
 *
 * 组件通过注入的端口使用显示、存储、串流和时钟能力，
 * 不反向依赖板卡全局对象（g_comp）或任何具体驱动实现。
 *
 * 所有函数指针可为 NULL 表示该能力不可用；组件必须按
 * 端口语义检查并返回明确错误，不能静默降级。
 */

#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* display port：显示独占租约 + 帧提交                                  */
/*                                                                     */
/* 租约语义（可行性报告 §4.8）：                                        */
/*   acquire() 必须阻止新的 LVGL flush、等待 in-flight flush/PPA/VSYNC   */
/*   完成后才返回 ESP_OK；期间只有组件 presenter 能提交 framebuffer。     */
/*   release() 必须在确认硬件不再访问 framebuffer 后归还显示所有权。      */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t phys_width;      /*!< 物理像素宽（Tab5：720） */
    uint16_t phys_height;     /*!< 物理像素高（Tab5：1280） */
    uint16_t logical_width;   /*!< 逻辑宽（旋转后，Tab5：1280） */
    uint16_t logical_height;  /*!< 逻辑高（旋转后，Tab5：720） */
    uint8_t  bytes_per_pixel; /*!< 物理帧缓冲字节/像素（RGB565：2） */
} otool_cubism_display_info_t;

typedef struct otool_cubism_display_port {
    void *ctx; /*!< 实现自有上下文 */

    /*! 获取显示独占租约。失败时组件不得进入 RUNNING。 */
    esp_err_t (*acquire)(void *ctx);

    /*! 归还显示独占租约；必须等待 DMA/PPA 静止。 */
    esp_err_t (*release)(void *ctx);

    /*! 提交一帧：将 inactive framebuffer 交给 presenter 显示。
     *  buffer：指向调用者提供的待显示像素；实现负责（或忽略）拷贝。 */
    esp_err_t (*present)(void *ctx, const void *buffer, size_t size_bytes);

    /*! 查询物理/逻辑显示信息。 */
    esp_err_t (*get_info)(void *ctx, otool_cubism_display_info_t *out);
} otool_cubism_display_port_t;

/* ------------------------------------------------------------------ */
/* storage port：有界文件访问                                          */
/* ------------------------------------------------------------------ */

typedef struct otool_cubism_storage_port {
    void *ctx;

    /*! 打开素材包 manifest 文件；成功返回非负句柄。 */
    esp_err_t (*open)(void *ctx, const char *path, int32_t *out_handle);

    /*! 读文件；返回实际读取字节数。 */
    esp_err_t (*read)(void *ctx, int32_t handle, void *buf, size_t size, size_t *out_read);

    /*! 定位；whence 语义同 fseek。 */
    esp_err_t (*seek)(void *ctx, int32_t handle, int64_t offset, int whence);

    /*! 查询文件大小。 */
    esp_err_t (*size)(void *ctx, int32_t handle, uint64_t *out_size);

    /*! 关闭文件。 */
    esp_err_t (*close)(void *ctx, int32_t handle);
} otool_cubism_storage_port_t;

/* ------------------------------------------------------------------ */
/* stream port：非阻塞读写（STREAM_CLIENT 使用）                        */
/* ------------------------------------------------------------------ */

typedef struct otool_cubism_stream_port {
    void *ctx;

    esp_err_t (*read)(void *ctx, void *buf, size_t size, size_t *out_read);
    esp_err_t (*write)(void *ctx, const void *buf, size_t size, size_t *out_written);
    esp_err_t (*is_connected)(void *ctx, bool *out_connected);
    esp_err_t (*cancel_wait)(void *ctx); /*!< 取消阻塞中的等待，用于 stop() */
} otool_cubism_stream_port_t;

/* ------------------------------------------------------------------ */
/* clock port：单调微秒时钟                                            */
/* ------------------------------------------------------------------ */

typedef struct otool_cubism_clock_port {
    void *ctx;
    int64_t (*now_us)(void *ctx); /*!< 单调递增微秒时间 */
} otool_cubism_clock_port_t;

#ifdef __cplusplus
} /* extern "C" */
#endif
