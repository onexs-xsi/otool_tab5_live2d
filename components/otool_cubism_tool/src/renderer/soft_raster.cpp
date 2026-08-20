/**
 * @file soft_raster.cpp
 * @brief otool_cubism_tool — renderer：最小软光栅实现（自研）
 */

#include "soft_raster.hpp"

namespace otool::cubism::renderer {
namespace {

inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* RGBA4444 预乘 alpha 混合到 RGB565（src 覆盖 dst） */
inline uint16_t blend_pma(uint16_t dst, uint16_t src)
{
    uint8_t sa = (uint8_t)(src & 0xF);
    if (sa == 0) {
        return dst;
    }
    if (sa == 0xF) {
        return src;
    }
    /* 预乘颜色 + alpha 混合 */
    uint8_t dr = (uint8_t)((dst >> 12) & 0xF);
    uint8_t dg = (uint8_t)((dst >> 8) & 0xF);
    uint8_t db = (uint8_t)((dst >> 4) & 0xF);
    uint8_t sr = (uint8_t)((src >> 12) & 0xF);
    uint8_t sg = (uint8_t)((src >> 8) & 0xF);
    uint8_t sb = (uint8_t)((src >> 4) & 0xF);
    /* out = src + dst*(1-sa)（预乘） */
    uint8_t r = (uint8_t)(sr + ((dr * (15 - sa)) >> 4));
    uint8_t g = (uint8_t)(sg + ((dg * (15 - sa)) >> 4));
    uint8_t b = (uint8_t)(sb + ((db * (15 - sa)) >> 4));
    uint8_t a = (uint8_t)(sa + ((0xF * (15 - sa)) >> 4));
    (void)a;
    return (uint16_t)((r << 12) | (g << 8) | (b << 4) | 0xF);
}

/* 重心坐标渲染单个三角形 */
void raster_tri(frame_buffer &fb, const texture_ref &tex,
                const float *v0, const float *v1, const float *v2,
                const float *uv0, const float *uv1, const float *uv2,
                float opacity)
{
    const float x0 = v0[0], y0 = v0[1];
    const float x1 = v1[0], y1 = v1[1];
    const float x2 = v2[0], y2 = v2[1];

    /* 面积（2 倍）与符号：背面剔除由调用方决定，这里统一按正面积处理 */
    float area2 = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (area2 == 0.0f) {
        return;
    }
    bool flip = area2 < 0.0f;
    if (flip) {
        area2 = -area2;
    }

    /* bounding box */
    int min_x = (int)(x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2));
    int max_x = (int)(x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2));
    int min_y = (int)(y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2));
    int max_y = (int)(y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2));
    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x >= fb.width) max_x = fb.width - 1;
    if (max_y >= fb.height) max_y = fb.height - 1;

    const uint16_t tex_w = tex.data ? tex.width : 1;
    const uint16_t tex_h = tex.data ? tex.height : 1;
    const uint8_t alpha = (uint8_t)(opacity * 15.0f + 0.5f);
    if (alpha == 0) {
        return;
    }

    for (int py = min_y; py <= max_y; ++py) {
        const float fy = (float)py + 0.5f;
        for (int px = min_x; px <= max_x; ++px) {
            const float fx = (float)px + 0.5f;
            /* 重心坐标 */
            float w0 = (x1 - x0) * (fy - y0) - (y1 - y0) * (fx - x0);
            float w1 = (x2 - x1) * (fy - y1) - (y2 - y1) * (fx - x1);
            float w2 = (x0 - x2) * (fy - y2) - (y0 - y2) * (fx - x2);
            if (flip) {
                w0 = -w0; w1 = -w1; w2 = -w2;
            }
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) {
                continue; /* 三角形外 */
            }
            float inv = 1.0f / area2;
            w0 *= inv; w1 *= inv; w2 *= inv;

            /* UV 插值（仿射） */
            float u = w0 * uv0[0] + w1 * uv1[0] + w2 * uv2[0];
            float v = w0 * uv0[1] + w1 * uv1[1] + w2 * uv2[1];
            if (u < 0.0f) u = 0.0f;
            if (u > 1.0f) u = 1.0f;
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;

            uint16_t src;
            if (tex.data != nullptr) {
                uint32_t tx = (uint32_t)(u * (float)(tex_w - 1) + 0.5f);
                uint32_t ty = (uint32_t)(v * (float)(tex_h - 1) + 0.5f);
                src = tex.data[ty * tex_w + tx];
                /* 应用整体不透明度：缩放 alpha 通道 */
                uint8_t sa = (uint8_t)(src & 0xF);
                if (alpha < 15) {
                    sa = (uint8_t)((sa * alpha) >> 4);
                    src = (uint16_t)((src & 0xFFF0) | sa);
                }
                if (sa == 0) {
                    continue;
                }
            } else {
                /* 无纹理：纯白 */
                src = (uint16_t)((15 << 12) | (15 << 8) | (15 << 4) | alpha);
            }

            uint16_t *dst = &fb.data[py * fb.width + px];
            *dst = blend_pma(*dst, src);
        }
    }
}

} // namespace

void fb_clear(frame_buffer &fb, uint16_t color)
{
    for (uint32_t i = 0; i < (uint32_t)fb.width * fb.height; ++i) {
        fb.data[i] = color;
    }
}

void draw_triangle(frame_buffer &fb, const texture_ref &tex,
                   const float *v0, const float *v1, const float *v2,
                   const float *uv0, const float *uv1, const float *uv2,
                   float opacity)
{
    raster_tri(fb, tex, v0, v1, v2, uv0, uv1, uv2, opacity);
}

void draw_mesh(frame_buffer &fb, const texture_ref &tex,
               const float *positions, const float *uvs,
               const uint16_t *indices,
               uint32_t vertex_count, uint32_t index_count,
               float opacity)
{
    (void)vertex_count;
    for (uint32_t i = 0; i + 2 < index_count; i += 3) {
        uint16_t a = indices[i], b = indices[i + 1], c = indices[i + 2];
        const float *pa = &positions[a * 2];
        const float *pb = &positions[b * 2];
        const float *pc = &positions[c * 2];
        const float *ta = &uvs[a * 2];
        const float *tb = &uvs[b * 2];
        const float *tc = &uvs[c * 2];
        raster_tri(fb, tex, pa, pb, pc, ta, tb, tc, opacity);
    }
}

} // namespace otool::cubism::renderer
