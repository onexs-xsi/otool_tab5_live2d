/**
 * @file model_render.cpp
 * @brief otool_cubism_tool — renderer：模型渲染器实现（自研）
 */

#include "model_render.hpp"

namespace otool::cubism::renderer {
namespace {

constexpr int32_t MAX_RENDER_DRAWABLES = 512;
constexpr int32_t MAX_MESH_VERTICES = 512;

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

void prepare_mesh_vertices(const model_render_input &in, uint32_t am,
                           float *scr, float *uv)
{
    const core::moc3_ir &ir = *in.ir;
    const float *src_pos = mesh_positions(*in.rt, am);
    const float *src_uv = mesh_uvs(ir, am);
    const int32_t vc = ir.i32(core::SLOT_AM_VERTEX_COUNT)[am];
    const float ppu = ir.info.pix_per_unit;

    for (int32_t j = 0; j < vc; ++j) {
        /*
         * Core 顶点以 pixels_per_unit 归一化，画布左上角还需加 origin。
         * self Core 的 y 已经是参考 Core y 的相反数，恰好对应屏幕向下的方向。
         */
        scr[j * 2 + 0] = (ir.info.origin_x + src_pos[j * 2 + 0] * ppu) *
                         in.scale + in.offset_x;
        scr[j * 2 + 1] = (ir.info.origin_y + src_pos[j * 2 + 1] * ppu) *
                         in.scale + in.offset_y;
        uv[j * 2 + 0] = src_uv[j * 2 + 0];
        uv[j * 2 + 1] = (ir.info.canvas_flag & 0x01U) == 0
                            ? (1.0f - src_uv[j * 2 + 1])
                            : src_uv[j * 2 + 1];
    }
}

const texture_ref &texture_for(const model_render_input &in, int32_t texture_no)
{
    static const texture_ref fallback = {nullptr, 1, 1};
    return texture_no >= 0 && (uint32_t)texture_no < in.texture_count
               ? in.textures[texture_no]
               : fallback;
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
    const int32_t *am_mask_begin = ir.i32(core::SLOT_AM_MASK_BEGIN);
    const int32_t *am_mask_count = ir.i32(core::SLOT_AM_MASK_COUNT);
    const int32_t *mask_pool = ir.i32(core::SLOT_MASK);
    const uint8_t *am_flags = ir.u8(core::SLOT_AM_DRAWABLE_FLAG);

    if (n_ams > MAX_RENDER_DRAWABLES) {
        return;
    }

    /* Cubism 文件中的 drawable 顺序不是绘制顺序；按本帧插值后的 order 稳定排序。 */
    uint16_t render_order[MAX_RENDER_DRAWABLES];
    for (int32_t i = 0; i < n_ams; ++i) {
        render_order[i] = (uint16_t)i;
    }
    for (int32_t i = 1; i < n_ams; ++i) {
        const uint16_t item = render_order[i];
        const float item_order = rt.mesh_draw_order[item];
        int32_t j = i;
        while (j > 0 && rt.mesh_draw_order[render_order[j - 1]] > item_order) {
            render_order[j] = render_order[j - 1];
            --j;
        }
        render_order[j] = item;
    }

    /* 最终顶点缓存（栈）：MVP 单 mesh 顶点上限 512 */
    float scr[MAX_MESH_VERTICES * 2];
    float uv[MAX_MESH_VERTICES * 2];
    alpha_buffer mask = in.mask;
    const bool has_mask_buffer = mask.data != nullptr &&
                                 mask.width == fb.width &&
                                 mask.height == fb.height;

    for (int32_t draw_idx = 0; draw_idx < n_ams; ++draw_idx) {
        const int32_t i = render_order[draw_idx];
        const int32_t vc = am_vc[i];
        const int32_t ic = am_ic[i];
        if (vc <= 0 || ic <= 0) {
            continue;
        }
        const float op = rt.mesh_opacity[i];
        if (op <= 0.0f) {
            continue;
        }
        const uint16_t *idx = mesh_indices(ir, (uint32_t)i);

        if (vc > MAX_MESH_VERTICES) {
            continue; /* MVP 上限 */
        }

        if (has_mask_buffer && am_mask_count[i] > 0) {
            alpha_clear(mask, 0, &fb);
            for (int32_t m = 0; m < am_mask_count[i]; ++m) {
                const int32_t mask_am = mask_pool[am_mask_begin[i] + m];
                if (mask_am < 0 || mask_am >= n_ams ||
                    am_vc[mask_am] <= 0 || am_vc[mask_am] > MAX_MESH_VERTICES ||
                    am_ic[mask_am] <= 0 || rt.mesh_opacity[mask_am] <= 0.0f) {
                    continue;
                }
                prepare_mesh_vertices(in, (uint32_t)mask_am, scr, uv);
                draw_mask_mesh(fb, mask,
                               texture_for(in, am_tex[mask_am]), scr, uv,
                               mesh_indices(ir, (uint32_t)mask_am),
                               (uint32_t)am_vc[mask_am], (uint32_t)am_ic[mask_am],
                               rt.mesh_opacity[mask_am]);
            }
            fb.clip_mask = mask.data;
            fb.clip_mask_inverted = (am_flags[i] & 0x08U) != 0;
        }

        prepare_mesh_vertices(in, (uint32_t)i, scr, uv);
        const mesh_blend_mode blend = (am_flags[i] & 0x01U) != 0
                                          ? mesh_blend_mode::additive
                                      : (am_flags[i] & 0x02U) != 0
                                          ? mesh_blend_mode::multiplicative
                                          : mesh_blend_mode::normal;
        draw_mesh(fb, texture_for(in, am_tex[i]),
                  scr, uv, idx, (uint32_t)vc, (uint32_t)ic, op, blend);
        fb.clip_mask = nullptr;
        fb.clip_mask_inverted = false;
    }
}

} // namespace otool::cubism::renderer
