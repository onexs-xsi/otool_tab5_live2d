/**
 * @file c0_probe.cpp
 * @brief otool_cubism_tool — host 工具：C0 校验探针（自研，不进入固件）
 *
 * 用与固件相同的 self Core 源码（moc3_common/moc3_reader/moc3_validate）
 * 在 PC 上对 moc3 文件执行 C0 inspect，输出模型信息或稳定错误码。
 *
 * 编译（Windows，VS Build Tools）：
 *   call vcvarsall.bat x64
 *   cl /nologo /std:c++17 /O2 /I <组件>/src/core/self c0_probe.cpp ^
 *      <组件>/src/core/self/moc3_validate.cpp /Fe:c0_probe.exe
 *
 * 用法： c0_probe.exe <file.moc3>
 */

#include "moc3_common.hpp"
#include "moc3_validate.hpp"
#include "moc3_ir.hpp"
#include "moc3_update.hpp"
#include "soft_raster.hpp"
#include "model_render.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace otool::cubism::core;
using namespace otool::cubism::renderer;

static const char *err_name(err_code c)
{
    switch (c) {
    case ERR_OK: return "OK";
    case ERR_NULL_POINTER: return "NULL_POINTER";
    case ERR_INVALID_SIZE: return "INVALID_SIZE";
    case ERR_BAD_ALIGNMENT: return "BAD_ALIGNMENT";
    case ERR_BAD_MAGIC: return "BAD_MAGIC";
    case ERR_UNSUPPORTED_VERSION: return "UNSUPPORTED_VERSION";
    case ERR_UNSUPPORTED_ENDIAN: return "UNSUPPORTED_ENDIAN";
    case ERR_UNKNOWN_FLAG: return "UNKNOWN_FLAG";
    case ERR_PROFILE_MISMATCH: return "PROFILE_MISMATCH";
    case ERR_TRUNCATED: return "TRUNCATED";
    case ERR_OFFSET_OVERFLOW: return "OFFSET_OVERFLOW";
    case ERR_SECTION_OVERLAP: return "SECTION_OVERLAP";
    case ERR_BAD_REFERENCE: return "BAD_REFERENCE";
    case ERR_CYCLE: return "CYCLE";
    case ERR_DEPTH_EXCEEDED: return "DEPTH_EXCEEDED";
    case ERR_CARDINALITY: return "CARDINALITY";
    case ERR_NAN_OR_INF: return "NAN_OR_INF";
    case ERR_KEY_NOT_MONOTONIC: return "KEY_NOT_MONOTONIC";
    case ERR_DUPLICATE_KEY: return "DUPLICATE_KEY";
    case ERR_LIMIT_EXCEEDED: return "LIMIT_EXCEEDED";
    case ERR_BUDGET_EXCEEDED: return "BUDGET_EXCEEDED";
    case ERR_ARENA_TOO_SMALL: return "ARENA_TOO_SMALL";
    case ERR_NO_MEMORY: return "NO_MEMORY";
    case ERR_INVALID_STATE: return "INVALID_STATE";
    case ERR_NOT_IMPLEMENTED: return "NOT_IMPLEMENTED";
    default: return "?";
    }
}

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); /* 无缓冲输出，便于崩溃定位 */
    if (argc < 2) {
        std::printf("usage: c0_probe <file.moc3> [--render <tex.raw> <tex.meta> <out.ppm>]\n");
        return 2;
    }
    const bool do_render = argc >= 6 && std::strcmp(argv[2], "--render") == 0;
    const char *tex_path = do_render ? argv[3] : nullptr;
    const char *tex_meta = do_render ? argv[4] : nullptr;
    const char *ppm_path = do_render ? argv[5] : nullptr;

    FILE *f = std::fopen(argv[1], "rb");
    if (f == nullptr) {
        std::printf("cannot open %s\n", argv[1]);
        return 2;
    }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (long)0xFFFFFFF) {
        std::printf("bad file size %ld\n", sz);
        std::fclose(f);
        return 2;
    }
    uint8_t *buf = (uint8_t *)std::malloc((size_t)sz);
    if (buf == nullptr) {
        std::printf("out of memory\n");
        std::fclose(f);
        return 2;
    }
    if (std::fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        std::printf("read failed\n");
        std::free(buf);
        std::fclose(f);
        return 2;
    }
    std::fclose(f);

    model_info_t info = {};
    err_info err = {};
    err_code rc = moc3_inspect(buf, (size_t)sz, &info, &err);

    std::printf("file: %s  size=%ld\n", argv[1], sz);
    std::printf("inspect -> %s (0x%x)\n", err_name(rc), (unsigned)rc);
    if (rc != ERR_OK) {
        std::printf("  section=%u offset=0x%x index=%u\n",
                    (unsigned)err.section_id, (unsigned)err.byte_offset,
                    (unsigned)err.index);
        std::free(buf);
        return 1;
    }

    std::printf("version=%u endian=%u section_count=%u\n",
                (unsigned)info.version, (unsigned)info.endian_flag,
                (unsigned)info.section_count);
    std::printf("canvas: pix_per_unit=%g origin=(%g,%g) size=%gx%g flag=0x%02x\n",
                info.pix_per_unit, info.origin_x, info.origin_y,
                info.canvas_width, info.canvas_height, (unsigned)info.canvas_flag);
    static const char *names[CI_COUNT] = {
        "parts", "deformers", "warps", "rotations", "art_meshes", "parameters",
        "part_kf", "warp_kf", "rotation_kf", "art_mesh_kf", "kf_pos",
        "axis_indices", "bindings", "axes", "keys", "uvs", "indices", "masks",
        "draw_groups", "draw_items", "glues", "glue_info", "glue_kf",
        "kf_mul_colors", "kf_scr_colors", "blend_axes", "blend_bindings",
        "bs_warps", "bs_art_meshes", "bs_constraint_idx", "bs_constraints",
        "bs_constraint_vals", "bs_parts", "bs_rotations", "bs_glues",
        "offscreens", "offscreen_kf", "bs_offscreens", "reserved",
    };
    for (int i = 0; i < CI_COUNT; ++i) {
        std::printf("  %-22s %d\n", names[i], info.counts.v[i]);
    }

    /* C1：构建不可变 IR + 静态校验 */
    moc3_ir ir = {};
    err_info ir_err = {};
    err_code ir_rc = moc3_build_ir(buf, (size_t)sz, &ir, &ir_err);
    std::printf("ir build -> %s (0x%x)\n", err_name(ir_rc), (unsigned)ir_rc);
    if (ir_rc != ERR_OK) {
        std::printf("  section=%u offset=0x%x index=%u\n",
                    (unsigned)ir_err.section_id, (unsigned)ir_err.byte_offset,
                    (unsigned)ir_err.index);
        std::free(buf);
        return 1;
    }

    /* IR 抽查：art_mesh[0] 与 param[0]（与 Python 探测对照） */
    const int32_t n_ams = ir.info.counts.v[CI_ART_MESHES];
    const int32_t n_params = ir.info.counts.v[CI_PARAMETERS];
    const int32_t *am_vc = ir.i32(SLOT_AM_VERTEX_COUNT);
    const int32_t *am_uv = ir.i32(SLOT_AM_UV_BEGIN);
    const int32_t *am_ib = ir.i32(SLOT_AM_IDX_BEGIN);
    const int32_t *am_ic = ir.i32(SLOT_AM_IDX_COUNT);
    const int32_t *am_kf = ir.i32(SLOT_AM_KF_OFF);
    const int32_t *am_kc = ir.i32(SLOT_AM_KF_COUNT);
    const float *param_max = ir.f32(SLOT_PARAM_MAX);
    const float *param_min = ir.f32(SLOT_PARAM_MIN);
    const float *param_def = ir.f32(SLOT_PARAM_DEFAULT);
    std::printf("ir: art_meshes=%d params=%d slots=%u\n",
                n_ams, n_params, (unsigned)ir.slot_count);
    if (n_ams > 0) {
        std::printf("  art_mesh[0]: vc=%d uv_begin=%d idx_begin=%d idx_cnt=%d kf=%d/%d\n",
                    am_vc[0], am_uv[0], am_ib[0], am_ic[0], am_kf[0], am_kc[0]);
        std::printf("  uv[0..3]: %g %g %g %g\n", ir.f32(SLOT_UV)[0], ir.f32(SLOT_UV)[1],
                    ir.f32(SLOT_UV)[2], ir.f32(SLOT_UV)[3]);
        std::printf("  indices[0..5]: %u %u %u %u %u %u\n",
                    ir.u16(SLOT_INDICES)[0], ir.u16(SLOT_INDICES)[1],
                    ir.u16(SLOT_INDICES)[2], ir.u16(SLOT_INDICES)[3],
                    ir.u16(SLOT_INDICES)[4], ir.u16(SLOT_INDICES)[5]);
    }
    if (n_params > 0) {
        std::printf("  param[0]: max=%g min=%g default=%g\n",
                    param_max[0], param_min[0], param_def[0]);
    }
    std::printf("  kf_pos[0..3]: %g %g %g %g\n",
                ir.f32(SLOT_KF_POS)[0], ir.f32(SLOT_KF_POS)[1],
                ir.f32(SLOT_KF_POS)[2], ir.f32(SLOT_KF_POS)[3]);
    /* rotation slot sanity (vs python probe) */
    std::printf("  rot: kf_off[0..2]=%d,%d,%d base_ang=%g origin_x[0..3]=%g,%g,%g,%g\n",
                ir.i32(SLOT_ROT_KF_OFF)[0], ir.i32(SLOT_ROT_KF_OFF)[1],
                ir.i32(SLOT_ROT_KF_OFF)[2],
                (double)ir.f32(SLOT_ROT_BASE_ANGLE)[0],
                (double)ir.f32(SLOT_ROT_KEY_ORIGIN_X)[0],
                (double)ir.f32(SLOT_ROT_KEY_ORIGIN_X)[1],
                (double)ir.f32(SLOT_ROT_KEY_ORIGIN_X)[2],
                (double)ir.f32(SLOT_ROT_KEY_ORIGIN_X)[3]);

    /* 渲染验证：640×360 场景 → PPM */
    if (do_render) {
        /* 读纹理 meta（JSON 中取 width/height） */
        FILE *mf = std::fopen(tex_meta, "rb");
        if (mf == nullptr) {
            std::printf("render: cannot open meta %s\n", tex_meta);
            std::free(buf);
            return 2;
        }
        char mjson[512] = {0};
        (void)std::fread(mjson, 1, sizeof(mjson) - 1, mf);
        std::fclose(mf);
        int tw = 0, th = 0;
        const char *kw = std::strstr(mjson, "\"width\"");
        const char *kh = std::strstr(mjson, "\"height\"");
        if (kw) std::sscanf(kw + 8, "%d", &tw);
        if (kh) std::sscanf(kh + 9, "%d", &th);
        if (tw <= 0 || th <= 0) {
            std::printf("render: bad meta (w=%d h=%d)\n", tw, th);
            std::free(buf);
            return 2;
        }
        FILE *tf = std::fopen(tex_path, "rb");
        if (tf == nullptr) {
            std::printf("render: cannot open texture %s\n", tex_path);
            std::free(buf);
            return 2;
        }
        std::fseek(tf, 0, SEEK_END);
        long tsz = std::ftell(tf);
        std::fseek(tf, 0, SEEK_SET);
        uint16_t *tex = (uint16_t *)std::malloc((size_t)tsz);
        if (tex == nullptr || std::fread(tex, 1, (size_t)tsz, tf) != (size_t)tsz) {
            std::printf("render: texture read failed\n");
            std::free(tex);
            std::fclose(tf);
            std::free(buf);
            return 2;
        }
        std::fclose(tf);

        constexpr uint16_t FB_W = 640, FB_H = 360;
        uint16_t *fb_data = (uint16_t *)std::calloc(FB_W * FB_H, 2);
        if (fb_data == nullptr) {
            std::printf("render: no memory\n");
            std::free(tex);
            std::free(buf);
            return 2;
        }
        frame_buffer fb = {fb_data, FB_W, FB_H};
        texture_ref texref = {tex, (uint16_t)tw, (uint16_t)th};

        /* 完整管线：runtime create → update（默认参数）→ render */
        core_runtime *rt = nullptr;
        err_code rc2 = core_runtime_create(&ir, &rt);
        std::printf("update: runtime_create -> %s (size=%u)\n",
                    err_name(rc2), (unsigned)core_runtime_size(&ir));
        if (rc2 != ERR_OK) {
            std::free(fb_data);
            std::free(tex);
            std::free(buf);
            return 1;
        }
        err_info uerr = {};
        err_code rc3 = core_update_frame(rt, nullptr, &uerr); /* 默认参数 */
        std::printf("update: frame (default params) -> %s (0x%x)\n",
                    err_name(rc3), (unsigned)rc3);
        if (rc3 != ERR_OK) {
            std::printf("  section=%u offset=0x%x index=%u\n",
                        (unsigned)uerr.section_id, (unsigned)uerr.byte_offset,
                        (unsigned)uerr.index);
        } else {
            const float *m0 = &rt->mesh_pos[rt->mesh_off[0] * 2];
            std::printf("update: am[0] final v0=(%g,%g) opacity=%g\n",
                        (double)m0[0], (double)m0[1],
                        (double)rt->mesh_opacity[0]);
            const int32_t n_ams = ir.info.counts.v[CI_ART_MESHES];
            int32_t visible = 0, inf_verts = 0;
            for (int32_t i = 0; i < n_ams; ++i) {
                const int32_t vc = ir.i32(SLOT_AM_VERTEX_COUNT)[i];
                if (rt->mesh_opacity[i] > 0.0f && vc > 0) ++visible;
                for (int32_t v = 0; v < vc && v < 4; ++v) {
                    const float *p = &rt->mesh_pos[(rt->mesh_off[i] + v) * 2];
                    if (!(p[0] > -100000.0f && p[0] < 100000.0f) ||
                        !(p[1] > -100000.0f && p[1] < 100000.0f)) {
                        ++inf_verts;
                    }
                }
            }
            std::printf("update: visible am=%d/%d inf_verts=%d\n", visible, n_ams, inf_verts);
            const float *m13 = &rt->mesh_pos[rt->mesh_off[13] * 2];
            std::printf("update: am[13] v0=(%g,%g) v1=(%g,%g)\n",
                        (double)m13[0], (double)m13[1],
                        (double)m13[2], (double)m13[3]);
            std::printf("update: rot[0] origin=(%g,%g) angle=%g scale=%g\n",
                        (double)rt->rot_origin_x[0], (double)rt->rot_origin_y[0],
                        (double)rt->rot_angle[0], (double)rt->rot_scale[0]);
            {
                const int32_t pdi = ir.i32(SLOT_AM_PARENT_DEF)[13];
                const int32_t pt = ir.i32(SLOT_DEF_TYPE)[pdi];
                const int32_t pl = ir.i32(SLOT_DEF_LOCAL)[pdi];
                std::printf("update: am[13] parent_def=%d type=%d local=%d\n",
                            (int)pdi, (int)pt, (int)pl);
                if (pt == 1 && pl >= 0 && pl < 60) {
                    std::printf("update: rot[%d] (composed) origin=(%g,%g) angle=%g scale=%g opa=%g\n",
                                (int)pl,
                                (double)rt->rot_origin_x[pl], (double)rt->rot_origin_y[pl],
                                (double)rt->rot_angle[pl], (double)rt->rot_scale[pl],
                                (double)rt->rot_opacity[pl]);
                }
            }
        }
        core_runtime_destroy(rt);

        /* 渲染（update 成功后） */
        model_render_input rin = {};
        rin.ir = &ir;
        rin.textures = &texref;
        rin.texture_count = 1;
        prepare_view(&rin, FB_W, FB_H);
        std::printf("render: view scale=%g offset=(%g,%g)\n",
                    rin.scale, rin.offset_x, rin.offset_y);
        if (rc3 == ERR_OK) {
            /* 重新 create/update（render 需要 rt） */
            rc2 = core_runtime_create(&ir, &rt);
            if (rc2 == ERR_OK) {
                rc3 = core_update_frame(rt, nullptr, &uerr);
                if (rc3 == ERR_OK) {
                    rin.rt = rt;
                    render_frame(rin, fb, 0x0000); /* 黑底 */

                    uint32_t lit = 0;
                    for (uint32_t i = 0; i < FB_W * FB_H; ++i) {
                        if (fb_data[i] != 0) ++lit;
                    }
                    std::printf("render: lit pixels=%u (%.1f%%)\n",
                                lit, 100.0 * (double)lit / (FB_W * FB_H));
                }
                core_runtime_destroy(rt);
            }
        }

        /* PPM P6 输出 */
        FILE *pf = std::fopen(ppm_path, "wb");
        if (pf == nullptr) {
            std::printf("render: cannot write %s\n", ppm_path);
            std::free(fb_data);
            std::free(tex);
            std::free(buf);
            return 2;
        }
        std::fprintf(pf, "P6\n%d %d\n255\n", FB_W, FB_H);
        uint8_t *rgb = (uint8_t *)std::malloc(FB_W * FB_H * 3);
        if (rgb == nullptr) {
            std::fclose(pf);
            std::free(fb_data);
            std::free(tex);
            std::free(buf);
            return 2;
        }
        for (uint32_t i = 0; i < FB_W * FB_H; ++i) {
            uint16_t p = fb_data[i];
            rgb[i * 3 + 0] = (uint8_t)(((p >> 11) & 0x1F) << 3);
            rgb[i * 3 + 1] = (uint8_t)(((p >> 5) & 0x3F) << 2);
            rgb[i * 3 + 2] = (uint8_t)((p & 0x1F) << 3);
        }
        std::fwrite(rgb, 1, FB_W * FB_H * 3, pf);
        std::fclose(pf);
        std::free(rgb);
        std::printf("render: wrote %s\n", ppm_path);

        std::free(fb_data);
        std::free(tex);
    }

    std::free(buf);
    return 0;
}
