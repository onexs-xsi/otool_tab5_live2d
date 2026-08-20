/**
 * @file model_render.hpp
 * @brief otool_cubism_tool — renderer：模型渲染器（自研）
 *
 * 渲染路径（可行性报告 §4.7 REALTIME 内部路径的雏形）：
 *   core runtime（update 后最终画布顶点/opacity）→ 画布→屏幕变换
 *     → 软光栅 → RGB565 场景缓冲
 *
 * 本轮范围：全部 drawable 直接渲染（mask 裁剪语义后续轮次）；
 * Normal 混合（alpha 混合），无 multiply/screen。
 */

#pragma once

#include "moc3_ir.hpp"
#include "moc3_update.hpp"
#include "soft_raster.hpp"

#include <cstdint>

namespace otool::cubism::renderer {

/** 渲染输入 */
struct model_render_input {
    const core::moc3_ir *ir;          /* C1 IR */
    const core::core_runtime *rt;     /* core_update_frame 后的顶点/opacity */

    const texture_ref *textures;      /* texture_no → 纹理 */
    uint32_t texture_count;

    /* 画布→屏幕变换（fit + 居中），由 prepare_view 计算 */
    float scale;
    float offset_x;
    float offset_y;
};

/** 计算 fit 变换（保持宽高比，居中） */
void prepare_view(model_render_input *in, uint16_t fb_w, uint16_t fb_h);

/** 渲染一帧到帧缓冲（先 clear 为背景色） */
void render_frame(const model_render_input &in, frame_buffer &fb,
                  uint16_t bg_color);

} // namespace otool::cubism::renderer
