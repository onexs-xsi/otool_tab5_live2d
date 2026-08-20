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

/** RGB565 帧缓冲 */
struct frame_buffer {
    uint16_t *data;
    uint16_t width;
    uint16_t height;
};

/** 清屏（填充单一 RGB565 颜色） */
void fb_clear(frame_buffer &fb, uint16_t color);

/** 绘制单三角形（屏幕坐标 + UV），nearest 采样 + 预乘 alpha 混合 */
void draw_triangle(frame_buffer &fb, const texture_ref &tex,
                   const float *v0, const float *v1, const float *v2,
                   const float *uv0, const float *uv1, const float *uv2,
                   float opacity);

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
               float opacity);

} // namespace otool::cubism::renderer
