/**
 * @file soft_raster.cpp
 * @brief otool_cubism_tool — renderer：最小软光栅实现（自研）
 */

#include "soft_raster.hpp"

#include <cmath>

namespace otool::cubism::renderer {
namespace {

inline uint16_t pack_rgb565(uint32_t r, uint32_t g, uint32_t b)
{
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* RGBA4444 预乘 alpha 混合到 RGB565（src 覆盖 dst） */
inline uint16_t blend_pma(uint16_t dst, uint16_t src)
{
    uint8_t sa = (uint8_t)(src & 0xF);
    if (sa == 0) {
        return dst;
    }

    const uint32_t inv = 15U - sa;
    const uint32_t dr = (dst >> 11) & 0x1FU;
    const uint32_t dg = (dst >> 5) & 0x3FU;
    const uint32_t db = dst & 0x1FU;
    const uint32_t sr = (src >> 12) & 0x0FU;
    const uint32_t sg = (src >> 8) & 0x0FU;
    const uint32_t sb = (src >> 4) & 0x0FU;

    /* src RGB 已预乘 alpha。直接在 RGB565 的各通道精度下合成并四舍五入。 */
    uint32_t r = (sr * 31U + dr * inv + 7U) / 15U;
    uint32_t g = (sg * 63U + dg * inv + 7U) / 15U;
    uint32_t b = (sb * 31U + db * inv + 7U) / 15U;
    if (r > 31U) r = 31U;
    if (g > 63U) g = 63U;
    if (b > 31U) b = 31U;
    return pack_rgb565(r, g, b);
}

inline uint16_t blend_add_pma(uint16_t dst, uint16_t src)
{
    uint32_t r = ((dst >> 11) & 0x1FU) + ((((src >> 12) & 0x0FU) * 31U + 7U) / 15U);
    uint32_t g = ((dst >> 5) & 0x3FU) + ((((src >> 8) & 0x0FU) * 63U + 7U) / 15U);
    uint32_t b = (dst & 0x1FU) + ((((src >> 4) & 0x0FU) * 31U + 7U) / 15U);
    if (r > 31U) r = 31U;
    if (g > 63U) g = 63U;
    if (b > 31U) b = 31U;
    return pack_rgb565(r, g, b);
}

inline uint16_t blend_multiply_pma(uint16_t dst, uint16_t src)
{
    const uint32_t inv = 15U - (src & 0x0FU);
    const uint32_t rf = ((src >> 12) & 0x0FU) + inv;
    const uint32_t gf = ((src >> 8) & 0x0FU) + inv;
    const uint32_t bf = ((src >> 4) & 0x0FU) + inv;
    uint32_t r = (((dst >> 11) & 0x1FU) * rf + 7U) / 15U;
    uint32_t g = (((dst >> 5) & 0x3FU) * gf + 7U) / 15U;
    uint32_t b = ((dst & 0x1FU) * bf + 7U) / 15U;
    if (r > 31U) r = 31U;
    if (g > 63U) g = 63U;
    if (b > 31U) b = 31U;
    return pack_rgb565(r, g, b);
}

inline uint16_t blend_pixel(uint16_t dst, uint16_t src, mesh_blend_mode mode)
{
    switch (mode) {
    case mesh_blend_mode::additive:
        return blend_add_pma(dst, src);
    case mesh_blend_mode::multiplicative:
        return blend_multiply_pma(dst, src);
    case mesh_blend_mode::normal:
    default:
        return blend_pma(dst, src);
    }
}

inline uint16_t scale_pma(uint16_t src, uint8_t opacity)
{
    if (opacity >= 15U) {
        return src;
    }
    const uint32_t r = (((src >> 12) & 0x0FU) * opacity + 7U) / 15U;
    const uint32_t g = (((src >> 8) & 0x0FU) * opacity + 7U) / 15U;
    const uint32_t b = (((src >> 4) & 0x0FU) * opacity + 7U) / 15U;
    const uint32_t a = ((src & 0x0FU) * opacity + 7U) / 15U;
    return (uint16_t)((r << 12) | (g << 8) | (b << 4) | a);
}

inline void write_sample(frame_buffer &fb, alpha_buffer *mask_target,
                         uint32_t pixel_index, uint16_t src,
                         mesh_blend_mode blend)
{
    if ((src & 0x0FU) == 0) {
        return;
    }
    if (mask_target != nullptr) {
        const uint32_t sa = (src & 0x0FU) * 17U;
        const uint32_t da = mask_target->data[pixel_index];
        mask_target->data[pixel_index] =
            (uint8_t)(sa + (da * (255U - sa) + 127U) / 255U);
        return;
    }

    if (fb.clip_mask != nullptr) {
        uint32_t ma = fb.clip_mask[pixel_index];
        if (fb.clip_mask_inverted) {
            ma = 255U - ma;
        }
        src = scale_pma(src, (uint8_t)((ma * 15U + 127U) / 255U));
        if ((src & 0x0FU) == 0) {
            return;
        }
    }

    uint16_t *dst = &fb.data[pixel_index];
    *dst = blend_pixel(*dst, src, blend);
}

inline void cooperate_after_row(frame_buffer &fb, uint32_t row_work)
{
    if (fb.cooperate == nullptr || fb.cooperate_interval == 0) {
        return;
    }
    fb.work_since_cooperate += row_work;
    if (fb.work_since_cooperate >= fb.cooperate_interval) {
        fb.work_since_cooperate = 0;
        fb.cooperate(fb.cooperate_ctx);
    }
}

/* 重心坐标渲染单个三角形 */
void raster_tri(frame_buffer &fb, const texture_ref &tex,
                const float *v0, const float *v1, const float *v2,
                const float *uv0, const float *uv1, const float *uv2,
                float opacity, mesh_blend_mode blend,
                alpha_buffer *mask_target)
{
    if (fb.data == nullptr || fb.width == 0 || fb.height == 0 ||
        v0 == nullptr || v1 == nullptr || v2 == nullptr ||
        uv0 == nullptr || uv1 == nullptr || uv2 == nullptr ||
        !std::isfinite(opacity) || opacity <= 0.0f) {
        return;
    }

    const float *p1 = v1;
    const float *p2 = v2;
    const float *t1 = uv1;
    const float *t2 = uv2;
    const float x0 = v0[0], y0 = v0[1];
    float x1 = p1[0], y1 = p1[1];
    float x2 = p2[0], y2 = p2[1];
    if (!std::isfinite(x0) || !std::isfinite(y0) ||
        !std::isfinite(x1) || !std::isfinite(y1) ||
        !std::isfinite(x2) || !std::isfinite(y2)) {
        return;
    }

    /* 统一为正绕序，后面的三条边函数在三角形内部都为非负。 */
    float area2 = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (!std::isfinite(area2) || std::fabs(area2) < 1.0e-6f) {
        return;
    }
    if (area2 < 0.0f) {
        const float *pt = p1; p1 = p2; p2 = pt;
        const float *tt = t1; t1 = t2; t2 = tt;
        x1 = p1[0]; y1 = p1[1];
        x2 = p2[0]; y2 = p2[1];
        area2 = -area2;
    }

    /* bounding box */
    const float min_fx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
    const float max_fx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    const float min_fy = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    const float max_fy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    int min_x = (int)std::floor(min_fx);
    int max_x = (int)std::ceil(max_fx) - 1;
    int min_y = (int)std::floor(min_fy);
    int max_y = (int)std::ceil(max_fy) - 1;
    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x >= fb.width) max_x = fb.width - 1;
    if (max_y >= fb.height) max_y = fb.height - 1;
    if (min_x > max_x || min_y > max_y) {
        return;
    }

    const bool textured = tex.data != nullptr && tex.width != 0 && tex.height != 0;
    const uint16_t tex_w = textured ? tex.width : 1;
    const uint16_t tex_h = textured ? tex.height : 1;
    if (opacity > 1.0f) opacity = 1.0f;
    const uint8_t alpha = (uint8_t)(opacity * 15.0f + 0.5f);
    if (alpha == 0) {
        return;
    }

    const float inv_area = 1.0f / area2;
    const float e01_dx = -(y1 - y0), e01_dy = x1 - x0;
    const float e12_dx = -(y2 - y1), e12_dy = x2 - x1;
    const float e20_dx = -(y0 - y2), e20_dy = x0 - x2;
    const float start_x = (float)min_x + 0.5f;
    const float start_y = (float)min_y + 0.5f;

    float row_e01 = (x1 - x0) * (start_y - y0) - (y1 - y0) * (start_x - x0);
    float row_e12 = (x2 - x1) * (start_y - y1) - (y2 - y1) * (start_x - x1);
    float row_e20 = (x0 - x2) * (start_y - y2) - (y0 - y2) * (start_x - x2);
    const uint32_t row_work = (uint32_t)(max_x - min_x + 1);

    for (int py = min_y; py <= max_y; ++py) {
        float e01 = row_e01;
        float e12 = row_e12;
        float e20 = row_e20;
        for (int px = min_x; px <= max_x; ++px) {
            if (e01 >= 0.0f && e12 >= 0.0f && e20 >= 0.0f) {
                /* e12/e20/e01 分别是顶点 0/1/2 的权重。 */
                const float w0 = e12 * inv_area;
                const float w1 = e20 * inv_area;
                const float w2 = e01 * inv_area;
                float u = w0 * uv0[0] + w1 * t1[0] + w2 * t2[0];
                float v = w0 * uv0[1] + w1 * t1[1] + w2 * t2[1];
                if (u < 0.0f) u = 0.0f;
                if (u > 1.0f) u = 1.0f;
                if (v < 0.0f) v = 0.0f;
                if (v > 1.0f) v = 1.0f;

                uint16_t src;
                if (textured) {
                    uint32_t tx = (uint32_t)(u * (float)(tex_w - 1) + 0.5f);
                    uint32_t ty = (uint32_t)(v * (float)(tex_h - 1) + 0.5f);
                    src = tex.data[ty * tex_w + tx];
                    /* PMA 纹理的颜色与 alpha 必须同时乘整体不透明度。 */
                    src = scale_pma(src, alpha);
                } else {
                    /* 无纹理：预乘 alpha 的纯白。 */
                    src = (uint16_t)((alpha << 12) | (alpha << 8) |
                                     (alpha << 4) | alpha);
                }
                const uint32_t pixel_index = (uint32_t)py * fb.width + (uint32_t)px;
                write_sample(fb, mask_target, pixel_index, src, blend);
            }
            e01 += e01_dx;
            e12 += e12_dx;
            e20 += e20_dx;
        }
        row_e01 += e01_dy;
        row_e12 += e12_dy;
        row_e20 += e20_dy;
        cooperate_after_row(fb, row_work);
    }
}

} // namespace

void fb_clear(frame_buffer &fb, uint16_t color)
{
    if (fb.data == nullptr) {
        return;
    }
    for (uint32_t y = 0; y < fb.height; ++y) {
        uint16_t *row = &fb.data[y * fb.width];
        for (uint32_t x = 0; x < fb.width; ++x) {
            row[x] = color;
        }
        cooperate_after_row(fb, fb.width);
    }
}

void alpha_clear(alpha_buffer &mask, uint8_t value, frame_buffer *schedule)
{
    if (mask.data == nullptr) {
        return;
    }
    for (uint32_t y = 0; y < mask.height; ++y) {
        uint8_t *row = &mask.data[y * mask.width];
        for (uint32_t x = 0; x < mask.width; ++x) {
            row[x] = value;
        }
        if (schedule != nullptr) {
            cooperate_after_row(*schedule, mask.width);
        }
    }
}

void draw_triangle(frame_buffer &fb, const texture_ref &tex,
                   const float *v0, const float *v1, const float *v2,
                   const float *uv0, const float *uv1, const float *uv2,
                   float opacity, mesh_blend_mode blend)
{
    raster_tri(fb, tex, v0, v1, v2, uv0, uv1, uv2, opacity, blend, nullptr);
}

void draw_mesh(frame_buffer &fb, const texture_ref &tex,
               const float *positions, const float *uvs,
               const uint16_t *indices,
               uint32_t vertex_count, uint32_t index_count,
               float opacity, mesh_blend_mode blend)
{
    if (positions == nullptr || uvs == nullptr || indices == nullptr) {
        return;
    }
    for (uint32_t i = 0; i + 2 < index_count; i += 3) {
        uint16_t a = indices[i], b = indices[i + 1], c = indices[i + 2];
        if ((uint32_t)a >= vertex_count || (uint32_t)b >= vertex_count ||
            (uint32_t)c >= vertex_count) {
            continue;
        }
        const float *pa = &positions[a * 2];
        const float *pb = &positions[b * 2];
        const float *pc = &positions[c * 2];
        const float *ta = &uvs[a * 2];
        const float *tb = &uvs[b * 2];
        const float *tc = &uvs[c * 2];
        raster_tri(fb, tex, pa, pb, pc, ta, tb, tc, opacity, blend, nullptr);
    }
}

void draw_mask_mesh(frame_buffer &schedule, alpha_buffer &mask,
                    const texture_ref &tex,
                    const float *positions, const float *uvs,
                    const uint16_t *indices,
                    uint32_t vertex_count, uint32_t index_count,
                    float opacity)
{
    if (mask.data == nullptr || mask.width != schedule.width ||
        mask.height != schedule.height || positions == nullptr ||
        uvs == nullptr || indices == nullptr) {
        return;
    }
    for (uint32_t i = 0; i + 2 < index_count; i += 3) {
        const uint16_t a = indices[i];
        const uint16_t b = indices[i + 1];
        const uint16_t c = indices[i + 2];
        if ((uint32_t)a >= vertex_count || (uint32_t)b >= vertex_count ||
            (uint32_t)c >= vertex_count) {
            continue;
        }
        raster_tri(schedule, tex,
                   &positions[a * 2], &positions[b * 2], &positions[c * 2],
                   &uvs[a * 2], &uvs[b * 2], &uvs[c * 2],
                   opacity, mesh_blend_mode::normal, &mask);
    }
}

} // namespace otool::cubism::renderer
