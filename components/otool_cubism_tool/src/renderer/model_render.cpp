/**
 * @file model_render.cpp
 * @brief otool_cubism_tool — renderer：模型渲染器实现（自研）
 */

#include "model_render.hpp"

namespace otool::cubism::renderer {
namespace {

inline const float *mesh_positions(const core::core_runtime &rt, uint32_t am)
{
    return &rt.mesh_pos[rt.mesh_off[am] * 2];
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
}

void render_frame(const model_render_input &in, frame_buffer &fb, uint16_t bg_color)
{
    fb_clear(fb, bg_color);

    const core::moc3_ir &ir = *in.ir;
    const core::core_runtime &rt = *in.rt;
    const int32_t n_ams = ir.info.counts.v[core::CI_ART_MESHES];
    if (n_ams <= 0 || in.textures == nullptr || in.texture_count == 0) {
        return;
    }

    const int32_t *am_vc = ir.i32(core::SLOT_AM_VERTEX_COUNT);
    const int32_t *am_ic = ir.i32(core::SLOT_AM_IDX_COUNT);
    const int32_t *am_tex = ir.i32(core::SLOT_AM_TEXTURE_NO);

    const float s = in.scale, ox = in.offset_x, oy = in.offset_y;

    /* 最终顶点缓存（栈）：MVP 单 mesh 顶点上限 512 */
    float scr[512 * 2];
    float uv[512 * 2];

    for (int32_t i = 0; i < n_ams; ++i) {
        const int32_t vc = am_vc[i];
        const int32_t ic = am_ic[i];
        if (vc <= 0 || ic <= 0) {
            continue;
        }
        const float op = rt.mesh_opacity[i];
        if (op <= 0.0f) {
            continue;
        }
        const int32_t tex_no = am_tex[i];
        const texture_ref *tex = (tex_no >= 0 && (uint32_t)tex_no < in.texture_count)
                                     ? &in.textures[tex_no]
                                     : nullptr;

        const float *src_pos = mesh_positions(rt, (uint32_t)i);
        const float *src_uv = mesh_uvs(ir, (uint32_t)i);
        const uint16_t *idx = mesh_indices(ir, (uint32_t)i);

        if ((uint32_t)vc > 512) {
            continue; /* MVP 上限 */
        }
        for (int32_t j = 0; j < vc; ++j) {
            scr[j * 2 + 0] = src_pos[j * 2 + 0] * s + ox;
            scr[j * 2 + 1] = src_pos[j * 2 + 1] * s + oy;
            /* UV：画布无 Y_REVERSED 时反转 v（与 Core 行为一致） */
            uv[j * 2 + 0] = src_uv[j * 2 + 0];
            uv[j * 2 + 1] = (in.ir->info.canvas_flag & 0x01) == 0
                                ? (1.0f - src_uv[j * 2 + 1])
                                : src_uv[j * 2 + 1];
        }

        const texture_ref fallback_tex = {nullptr, 1, 1};
        draw_mesh(fb, tex != nullptr ? *tex : fallback_tex,
                  scr, uv, idx, (uint32_t)vc, (uint32_t)ic, op);
    }
}

} // namespace otool::cubism::renderer
