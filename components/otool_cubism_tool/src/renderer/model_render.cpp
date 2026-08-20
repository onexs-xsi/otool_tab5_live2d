/**
 * @file model_render.cpp
 * @brief otool_cubism_tool — renderer：模型渲染器实现（自研）
 */

#include "model_render.hpp"

#include <cstdio>

namespace otool::cubism::renderer {
namespace {

#ifndef OTOOL_CUBISM_RENDER_DEBUG_PRINT
#define OTOOL_CUBISM_RENDER_DEBUG_PRINT 0
#endif

/** 取某 art_mesh 的 keyform 位置池（本轮固定 keyform 0 = 组合 0） */
inline const float *mesh_positions(const core::moc3_ir &ir, uint32_t am)
{
    const int32_t *kf_off = ir.i32(core::SLOT_AM_KF_OFF);
    const int32_t *key_pos = ir.i32(core::SLOT_AM_KEY_POS_OFF);
    const float *kf_pool = ir.f32(core::SLOT_KF_POS);
    int32_t po = key_pos[kf_off[am] + 0];
    return &kf_pool[po * 2];
}

inline const float *mesh_uvs(const core::moc3_ir &ir, uint32_t am)
{
    const int32_t *uv_begin = ir.i32(core::SLOT_AM_UV_BEGIN);
    const float *uv_pool = ir.f32(core::SLOT_UV);
    return &uv_pool[uv_begin[am] * 2];
}

inline const uint16_t *mesh_indices(const core::moc3_ir &ir, uint32_t am)
{
    const int32_t *idx_begin = ir.i32(core::SLOT_AM_IDX_BEGIN);
    const uint16_t *idx_pool = ir.u16(core::SLOT_INDICES);
    return &idx_pool[idx_begin[am]];
}

} // namespace

void prepare_view(model_render_input *in, uint16_t fb_w, uint16_t fb_h)
{
    const core::model_info_t &info = in->ir->info;
    const float cw = info.canvas_width;
    const float ch = info.canvas_height;
    const float s = cw > 0.0f && ch > 0.0f
                        ? (fb_w / cw < fb_h / ch ? fb_w / cw : fb_h / ch)
                        : 1.0f;
    in->scale = s;
    in->offset_x = ((float)fb_w - cw * s) * 0.5f;
    in->offset_y = ((float)fb_h - ch * s) * 0.5f;
    /* 画布 flag 位 0 = Y_REVERSED；未设置时 UV 需反转（PurismCore 行为研究） */
    in->flip_uv_y = (info.canvas_flag & 0x01) == 0;
}

void render_frame(const model_render_input &in, frame_buffer &fb, uint16_t bg_color)
{
    fb_clear(fb, bg_color);

    const core::moc3_ir &ir = *in.ir;
    const int32_t n_ams = ir.info.counts.v[core::CI_ART_MESHES];
    if (n_ams <= 0 || in.textures == nullptr || in.texture_count == 0) {
        return;
    }

    const int32_t *am_vc = ir.i32(core::SLOT_AM_VERTEX_COUNT);
    const int32_t *am_ic = ir.i32(core::SLOT_AM_IDX_COUNT);
    const int32_t *am_tex = ir.i32(core::SLOT_AM_TEXTURE_NO);

    const float s = in.scale, ox = in.offset_x, oy = in.offset_y;
    const bool flip = in.flip_uv_y;

    for (int32_t i = 0; i < n_ams; ++i) {
#if OTOOL_CUBISM_RENDER_DEBUG_PRINT
        if ((i & 0x7) == 0) {
            std::printf("render_frame: am[%d]/%d\n", (int)i, (int)n_ams);
        }
#endif
        const int32_t vc = am_vc[i];
        const int32_t ic = am_ic[i];
        if (vc <= 0 || ic <= 0) {
            continue;
        }
        const int32_t tex_no = am_tex[i];
        const texture_ref *tex = (tex_no >= 0 && (uint32_t)tex_no < in.texture_count)
                                     ? &in.textures[tex_no]
                                     : nullptr;

        const float *src_pos = mesh_positions(ir, (uint32_t)i);
        const float *src_uv = mesh_uvs(ir, (uint32_t)i);
        const uint16_t *idx = mesh_indices(ir, (uint32_t)i);
#if OTOOL_CUBISM_RENDER_DEBUG_PRINT
        std::printf("render_frame: am[%d] vc=%d ic=%d tex=%d kf0_pos=%.1f,%.1f\n",
                    (int)i, (int)vc, (int)ic, (int)tex_no,
                    (double)src_pos[0], (double)src_pos[1]);
#endif

        /* 变换顶点到屏幕坐标（栈缓冲：MVP 顶点数上限 512） */
        float scr[512 * 2];
        if ((uint32_t)vc > 512) {
            continue; /* 超出 MVP 上限，跳过（hard limit 未来收紧） */
        }
        for (int32_t j = 0; j < vc; ++j) {
            scr[j * 2 + 0] = src_pos[j * 2 + 0] * s + ox;
            scr[j * 2 + 1] = src_pos[j * 2 + 1] * s + oy;
        }

        /* UV（flip 时反转 v） */
        float uv[512 * 2];
        for (int32_t j = 0; j < vc; ++j) {
            uv[j * 2 + 0] = src_uv[j * 2 + 0];
            uv[j * 2 + 1] = flip ? (1.0f - src_uv[j * 2 + 1]) : src_uv[j * 2 + 1];
        }

        const texture_ref fallback_tex = {nullptr, 1, 1};
        draw_mesh(fb, tex != nullptr ? *tex : fallback_tex,
                  scr, uv, idx, (uint32_t)vc, (uint32_t)ic, 1.0f);
    }
}

} // namespace otool::cubism::renderer
