/**
 * @file soft_raster.hpp
 * @brief otool_cubism_tool — renderer：最小软光栅（自研）
 *
 * 最小可行光栅（可行性报告 §4.9 要点，逐步增强）：
 *   - bounding-box + 重心坐标（float，先正确后优化）
 *   - RGBA4444 预乘 alpha 纹理，nearest 采样
 *   - 输出 RGB565 帧缓冲
 *   - 无透视校正（2D 网格 UV 仿射插值即可）
 */

#pragma once

#include <cstdint>
#include <cstddef>

namespace otool::cubism::renderer {

/** RGBA4444 纹理（预乘 alpha，小端 u16：R4G4B4A4） */
struct texture_ref {
    const uint16_t *data; /* 可为空（纯色） */
    uint16_t width;
    uint16_t height;
};

enum class mesh_blend_mode : uint8_t {
    normal = 0,
    additive,
    multiplicative,
};

/**
 * 长光栅任务的协作回调。回调在完整扫描行之间触发，绝不会在像素写入中途触发。
 * ESP 端可在回调里短暂阻塞，让 IDLE task 和显示任务获得运行机会；host 端可留空。
 */
using raster_cooperate_fn = void (*)(void *ctx);

/** RGB565 帧缓冲 */
struct frame_buffer {
    uint16_t *data;
    uint16_t width;
    uint16_t height;

    raster_cooperate_fn cooperate = nullptr;
    void *cooperate_ctx = nullptr;
    uint32_t cooperate_interval = 0;      /* 检查过的 bbox 像素数；0 = 禁用 */
    uint32_t work_since_cooperate = 0;

    const uint8_t *clip_mask = nullptr;   /* 与 framebuffer 同尺寸的 A8 蒙版 */
    bool clip_mask_inverted = false;
};

struct alpha_buffer {
    uint8_t *data;
    uint16_t width;
    uint16_t height;
};

/** 清屏（填充单一 RGB565 颜色） */
void fb_clear(frame_buffer &fb, uint16_t color);

/** 清空 A8 蒙版；schedule 可选，用于沿用 RGB framebuffer 的协作回调。 */
void alpha_clear(alpha_buffer &mask, uint8_t value, frame_buffer *schedule = nullptr);

/** 绘制单三角形（屏幕坐标 + UV），nearest 采样 + 预乘 alpha 混合 */
void draw_triangle(frame_buffer &fb, const texture_ref &tex,
                   const float *v0, const float *v1, const float *v2,
                   const float *uv0, const float *uv1, const float *uv2,
                   float opacity,
                   mesh_blend_mode blend = mesh_blend_mode::normal);

/**
 * @brief 绘制带纹理的三角形带（每 3 个索引一个三角形）
 * @param fb         目标帧缓冲
 * @param tex        纹理
 * @param positions  顶点（x,y 屏幕坐标），2×vertex_count 个 float
 * @param uvs        UV（u,v 纹理坐标，v 已按渲染约定处理），2×vertex_count
 * @param indices    索引（每 3 个一组）
 * @param vertex_count 顶点数
 * @param index_count  索引总数（3 的倍数）
 * @param opacity    整体不透明度（1.0 = 不透明）
 */
void draw_mesh(frame_buffer &fb, const texture_ref &tex,
               const float *positions, const float *uvs,
               const uint16_t *indices,
               uint32_t vertex_count, uint32_t index_count,
               float opacity,
               mesh_blend_mode blend = mesh_blend_mode::normal);

/** 把 mesh 的纹理 alpha 以 src-over/union 语义写入 A8 蒙版。 */
void draw_mask_mesh(frame_buffer &schedule, alpha_buffer &mask,
                    const texture_ref &tex,
                    const float *positions, const float *uvs,
                    const uint16_t *indices,
                    uint32_t vertex_count, uint32_t index_count,
                    float opacity);

} // namespace otool::cubism::renderer
