/**
 * @file otool_cubism_demo.h
 * @brief otool_cubism_tool — 静态模型演示 API（S1 过渡，自研）
 *
 * 用途：在 main 中快速把嵌入式 moc3 + 纹理素材渲染到 RGB565 帧缓冲，
 * 由上层（LVGL image 等）负责上屏。这是组件内 self Core 的薄封装：
 * main 不直接包含 core/renderer 私有头文件。
 *
 * 素材规则（2026-08-21）：演示素材（Mao.moc3 / 打包纹理）为第三方研究材料，
 * 只允许从 %TEMP%/otool_cubism_research 构建期嵌入，不进入仓库。
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace otool::cubism::demo {

/** 嵌入式模型素材（指针由调用方持有，生命周期须覆盖模型句柄） */
struct model_asset {
    const uint8_t *moc3;     /*!< moc3 文件字节（构建期嵌入，flash 只读区） */
    size_t         moc3_size;
    const uint8_t *tex;      /*!< RGBA4444 小端原始纹理（预乘 alpha） */
    uint16_t       tex_w;
    uint16_t       tex_h;
};

struct model_handle;

/** 创建模型：拷贝 moc3 到对齐缓冲 + 构建 IR/runtime + 拷贝纹理到 PSRAM。
 *  失败返回 nullptr。 */
model_handle *model_create(const model_asset &asset);

/** 释放模型全部资源（含 moc3 副本、纹理副本、runtime）。 */
void model_destroy(model_handle *h);

/** 参数个数（ir.parameters）；越界索引由 set_param 忽略。 */
int model_param_count(const model_handle *h);

/** 覆盖某个参数值（不 clamp，update 内部会 clamp）。 */
void model_set_param(model_handle *h, int idx, float value);

/** 执行一帧 update + 渲染到 rgb565（fb_w*fb_h 像素，RGB565 小端）。
 *  返回 false 表示 update 失败。 */
bool model_render(model_handle *h, uint16_t *rgb565, uint16_t fb_w, uint16_t fb_h);

} // namespace otool::cubism::demo
