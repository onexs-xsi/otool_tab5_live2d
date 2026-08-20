/**
 * @file moc3_update.cpp
 * @brief otool_cubism_tool - self Core: C2+C4 minimal update (self-authored)
 *
 * Algorithm written independently from behavior research of reference
 * implementations (docs/research_log.md round 4), not copied code.
 *
 * Coordinate semantics (research findings):
 *   - top-level warp grid vertices = canvas coordinates (absolute)
 *   - nested warp grid vertices = parent-space normalized coords [0,1]
 *   - art_mesh keyform vertices = parent deformer space local coords
 */

#include "moc3_update.hpp"
#include "moc3_reader.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace otool::cubism::core {
namespace {

constexpr uint16_t SEC_GENERAL = 0xFFFF; /* generic error section */
constexpr uint32_t MAX_COMBO = 16;        /* 4 axes max (2^4) */
constexpr uint32_t MAX_WARP_VERTEX = 64;  /* MVP grid vertex cap */
constexpr uint32_t MAX_DEFORMERS = 512;
constexpr uint32_t MAX_WARPS = 256;
constexpr float SNAP_EPS = 1e-5f;

inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* key search: value -> segment [keys[idx], keys[idx+1]) idx and weight */
struct key_seg {
    int32_t idx;
    float weight;
};

key_seg find_key_segment(float value, const float *keys, int32_t key_count)
{
    key_seg r = {0, 0.0f};
    if (key_count <= 1) {
        r.idx = 0;
        return r;
    }
    if (value <= keys[0] + SNAP_EPS) {
        r.idx = 0;
        return r;
    }
    const float last = keys[key_count - 1];
    if (value >= last - SNAP_EPS) {
        r.idx = key_count - 1;
        return r;
    }
    for (int32_t k = 0; k + 1 < key_count; ++k) {
        if (value < keys[k + 1] - SNAP_EPS) {
            r.idx = k;
            const float diff = keys[k + 1] - keys[k];
            if (diff > 1e-6f) {
                r.weight = (value - keys[k]) / diff;
            }
            return r;
        }
        if (value < keys[k + 1] + SNAP_EPS) {
            r.idx = k + 1;
            return r;
        }
    }
    r.idx = key_count - 1;
    return r;
}

/* vertex-array keyform blend (each keyform is a vertex block) */
/* pos_off_pool entries are f32 indices into pos_pool (x,y pairs) */
void blend_keyform_positions(const float *pos_pool, const int32_t *pos_off_pool,
                             int32_t kf_off, const int32_t *kf_idx,
                             const float *kf_w, int32_t blend_count,
                             int32_t vertex_count, float *out)
{
    std::memset(out, 0, (size_t)vertex_count * 2 * sizeof(float));
    for (int32_t j = 0; j < blend_count; ++j) {
        const float w = kf_w != nullptr ? kf_w[j] : 1.0f;
        if (w == 0.0f) {
            continue;
        }
        const int32_t ki = kf_idx != nullptr ? kf_idx[j] : 0;
        const int32_t po = pos_off_pool[kf_off + ki];
        const float *src = &pos_pool[po];
        for (int32_t v = 0; v < vertex_count * 2; ++v) {
            out[v] += src[v] * w;
        }
    }
}

/* scalar keyform blend */
float blend_keyform_scalar(const float *vals, int32_t kf_off,
                           const int32_t *kf_idx, const float *kf_w,
                           int32_t blend_count)
{
    float acc = 0.0f;
    for (int32_t j = 0; j < blend_count; ++j) {
        const float w = kf_w != nullptr ? kf_w[j] : 1.0f;
        const int32_t ki = kf_idx != nullptr ? kf_idx[j] : 0;
        acc += vals[kf_off + ki] * w;
    }
    return acc;
}

/* ---- deformer transforms ---- */

/* warp: local coord (0..1 normalized) -> grid interpolation
 * (quad: bilinear / tri: diagonal split) */
void warp_transform_point(const float *grid, int32_t row, int32_t col,
                          float u, float v, bool quad, float *out)
{
    const int32_t stride = col + 1;
    const float fr = (float)row, fc = (float)col;
    const float gu = u * fc, gv = v * fr;

    if (u >= 0.0f && u < 1.0f && v >= 0.0f && v < 1.0f) {
        int32_t cu = (int32_t)gu, cv = (int32_t)gv;
        const float fu = gu - (float)cu, fv = gv - (float)cv;
        const int32_t bi = cv * stride + cu;
        const float *p00 = &grid[bi * 2];
        const float *p10 = &grid[(bi + 1) * 2];
        const float *p01 = &grid[(bi + stride) * 2];
        const float *p11 = &grid[(bi + stride + 1) * 2];
        if (quad) {
            const float w00 = (1.0f - fu) * (1.0f - fv);
            const float w10 = fu * (1.0f - fv);
            const float w01 = (1.0f - fu) * fv;
            const float w11 = fu * fv;
            out[0] = p00[0] * w00 + p10[0] * w10 + p01[0] * w01 + p11[0] * w11;
            out[1] = p00[1] * w00 + p10[1] * w10 + p01[1] * w01 + p11[1] * w11;
        } else {
            if (fu + fv <= 1.0f) {
                const float w0 = 1.0f - fu - fv;
                out[0] = p00[0] * w0 + p10[0] * fu + p01[0] * fv;
                out[1] = p00[1] * w0 + p10[1] * fu + p01[1] * fv;
            } else {
                const float w0 = 1.0f - fv;
                const float w1 = fu + fv - 1.0f;
                const float w2 = 1.0f - fu;
                out[0] = p10[0] * w0 + p11[0] * w1 + p01[0] * w2;
                out[1] = p10[1] * w0 + p11[1] * w1 + p01[1] * w2;
            }
        }
        return;
    }
    /* MVP: clamp out-of-range vertices to grid edge (extrapolation later) */
    const float cu = clampf(u, 0.0f, 1.0f) * fc;
    const float cv = clampf(v, 0.0f, 1.0f) * fr;
    const int32_t icu = (int32_t)(cu < fc ? cu : fc - 1.0f);
    const int32_t icv = (int32_t)(cv < fr ? cv : fr - 1.0f);
    const int32_t bi = icv * stride + icu;
    out[0] = grid[bi * 2];
    out[1] = grid[bi * 2 + 1];
}

/* rotation: affine transform */
void rot_transform_point(float angle_deg, float scale,
                         float origin_x, float origin_y,
                         float x, float y, float *out)
{
    const float a = angle_deg * 3.14159265358979323846f / 180.0f;
    const float sa = sinf(a), ca = cosf(a);
    const float m00 = scale * ca;
    const float m01 = scale * (-sa);
    const float m10 = scale * sa;
    const float m11 = scale * ca;
    out[0] = m00 * x + m01 * y + origin_x;
    out[1] = m10 * x + m11 * y + origin_y;
}

} // namespace

uint32_t core_runtime_size(const moc3_ir *ir)
{
    const count_info_t &c = ir->info.counts;
    uint32_t total_am = 0, total_warp = 0;
    const int32_t *am_vc = ir->i32(SLOT_AM_VERTEX_COUNT);
    const int32_t *warp_row = ir->i32(SLOT_WARP_ROW);
    const int32_t *warp_col = ir->i32(SLOT_WARP_COLUMN);
    for (int32_t i = 0; i < c.v[CI_ART_MESHES]; ++i) {
        if (am_vc[i] > 0) total_am += (uint32_t)am_vc[i];
    }
    for (int32_t i = 0; i < c.v[CI_WARPS]; ++i) {
        if (warp_row[i] > 0 && warp_col[i] > 0) {
            total_warp += (uint32_t)(warp_row[i] + 1) * (uint32_t)(warp_col[i] + 1);
        }
    }
    const size_t blend_count_bytes =
        ((size_t)c.v[CI_BINDINGS] + alignof(float) - 1U) & ~(alignof(float) - 1U);
    return (uint32_t)(sizeof(core_runtime) +
                      (size_t)c.v[CI_PARAMETERS] * sizeof(float) +
                      (size_t)c.v[CI_AXES] * (sizeof(int32_t) + sizeof(float)) +
                      (size_t)c.v[CI_BINDINGS] * (16 * sizeof(int32_t) + 16 * sizeof(float)) +
                      blend_count_bytes +
                      (size_t)total_warp * 2 * sizeof(float) +
                      (size_t)c.v[CI_ROTATIONS] * 5 * sizeof(float) +
                      (size_t)c.v[CI_WARPS] * sizeof(float) +
                      (size_t)total_am * 2 * sizeof(float) +
                      (size_t)c.v[CI_ART_MESHES] * 2 * sizeof(float) +
                      (size_t)(c.v[CI_ART_MESHES] + 1) * sizeof(uint32_t));
}

err_code core_runtime_create(const moc3_ir *ir, core_runtime **out_rt)
{
    if (ir == nullptr || out_rt == nullptr) {
        return ERR_NULL_POINTER;
    }
    const uint32_t total = core_runtime_size(ir);
    uint8_t *mem = (uint8_t *)std::malloc(total);
    if (mem == nullptr) {
        return ERR_NO_MEMORY;
    }
    std::memset(mem, 0, total);

    core_runtime *rt = (core_runtime *)mem;
    uint8_t *p = mem + sizeof(core_runtime);
    const count_info_t &c = ir->info.counts;

    rt->ir = ir;
    rt->param_value = (float *)p; p += (size_t)c.v[CI_PARAMETERS] * sizeof(float);
    rt->axis_idx = (int32_t *)p; p += (size_t)c.v[CI_AXES] * sizeof(int32_t);
    rt->axis_weight = (float *)p; p += (size_t)c.v[CI_AXES] * sizeof(float);
    rt->bind_keyform_idx = (int32_t *)p; p += (size_t)c.v[CI_BINDINGS] * 16 * sizeof(int32_t);
    rt->bind_weights = (float *)p; p += (size_t)c.v[CI_BINDINGS] * 16 * sizeof(float);
    rt->bind_blend_count = (uint8_t *)p; p += (size_t)c.v[CI_BINDINGS];
    p = (uint8_t *)(((uintptr_t)p + alignof(float) - 1U) & ~(uintptr_t)(alignof(float) - 1U));

    const int32_t *am_vc = ir->i32(SLOT_AM_VERTEX_COUNT);
    const int32_t *warp_row = ir->i32(SLOT_WARP_ROW);
    const int32_t *warp_col = ir->i32(SLOT_WARP_COLUMN);
    uint32_t total_warp = 0, total_am = 0;
    for (int32_t i = 0; i < c.v[CI_WARPS]; ++i) {
        if (warp_row[i] > 0 && warp_col[i] > 0) {
            total_warp += (uint32_t)(warp_row[i] + 1) * (uint32_t)(warp_col[i] + 1);
        }
    }
    for (int32_t i = 0; i < c.v[CI_ART_MESHES]; ++i) {
        if (am_vc[i] > 0) total_am += (uint32_t)am_vc[i];
    }
    rt->total_warp_vertices = total_warp;
    rt->total_mesh_vertices = total_am;

    rt->warp_pos = (float *)p; p += (size_t)total_warp * 2 * sizeof(float);
    rt->rot_angle = (float *)p; p += (size_t)c.v[CI_ROTATIONS] * sizeof(float);
    rt->rot_origin_x = (float *)p; p += (size_t)c.v[CI_ROTATIONS] * sizeof(float);
    rt->rot_origin_y = (float *)p; p += (size_t)c.v[CI_ROTATIONS] * sizeof(float);
    rt->rot_scale = (float *)p; p += (size_t)c.v[CI_ROTATIONS] * sizeof(float);
    rt->rot_opacity = (float *)p; p += (size_t)c.v[CI_ROTATIONS] * sizeof(float);
    rt->warp_opacity = (float *)p; p += (size_t)c.v[CI_WARPS] * sizeof(float);
    rt->mesh_pos = (float *)p; p += (size_t)total_am * 2 * sizeof(float);
    rt->mesh_opacity = (float *)p; p += (size_t)c.v[CI_ART_MESHES] * sizeof(float);
    rt->mesh_draw_order = (float *)p; p += (size_t)c.v[CI_ART_MESHES] * sizeof(float);
    rt->mesh_off = (uint32_t *)p; p += (size_t)(c.v[CI_ART_MESHES] + 1) * sizeof(uint32_t);

    uint32_t acc = 0;
    for (int32_t i = 0; i < c.v[CI_ART_MESHES]; ++i) {
        rt->mesh_off[i] = acc;
        if (am_vc[i] > 0) acc += (uint32_t)am_vc[i];
    }
    rt->mesh_off[c.v[CI_ART_MESHES]] = acc;

    *out_rt = rt;
    return ERR_OK;
}

void core_runtime_destroy(core_runtime *rt)
{
    std::free(rt);
}

err_code core_update_frame(core_runtime *rt, const float *params, err_info *err)
{
    if (rt == nullptr || rt->ir == nullptr) {
        err_set(err, ERR_NULL_POINTER, SEC_GENERAL, 0);
        return ERR_NULL_POINTER;
    }
    const moc3_ir &ir = *rt->ir;
    const count_info_t &c = ir.info.counts;
    const int32_t n_params = c.v[CI_PARAMETERS];
    const int32_t n_axes = c.v[CI_AXES];
    const int32_t n_binds = c.v[CI_BINDINGS];
    const int32_t n_warps = c.v[CI_WARPS];
    const int32_t n_rots = c.v[CI_ROTATIONS];
    const int32_t n_ams = c.v[CI_ART_MESHES];
    const int32_t n_defs = c.v[CI_DEFORMERS];

    /* ---- C2a: parameter clamp (no repeat in MVP) ---- */
    const float *p_max = ir.f32(SLOT_PARAM_MAX);
    const float *p_min = ir.f32(SLOT_PARAM_MIN);
    const float *p_def = ir.f32(SLOT_PARAM_DEFAULT);
    const int32_t *p_repeat = ir.i32(SLOT_PARAM_REPEAT);
    for (int32_t i = 0; i < n_params; ++i) {
        if (p_repeat[i] != 0) {
            err_set(err, ERR_NOT_IMPLEMENTED, SLOT_PARAM_REPEAT, 0, (uint32_t)i);
            return ERR_NOT_IMPLEMENTED;
        }
        float v = params != nullptr ? params[i] : p_def[i];
        rt->param_value[i] = clampf(v, p_min[i], p_max[i]);
    }

    /* ---- C2b: axis key search ---- */
    const int32_t *p_axis_begin = ir.i32(SLOT_PARAM_AXIS_BEGIN);
    const int32_t *p_axis_cnt = ir.i32(SLOT_PARAM_AXIS_COUNT);
    const int32_t *axis_keys_begin = ir.i32(SLOT_AXIS_KEYS_BEGIN);
    const int32_t *axis_keys_cnt = ir.i32(SLOT_AXIS_KEYS_COUNT);
    const float *keys_pool = ir.f32(SLOT_KEYS);
    for (int32_t i = 0; i < n_params; ++i) {
        for (int32_t j = 0; j < p_axis_cnt[i]; ++j) {
            const int32_t a = p_axis_begin[i] + j;
            if (a < 0 || a >= n_axes) {
                err_set(err, ERR_BAD_REFERENCE, SLOT_PARAM_AXIS_BEGIN, 0, (uint32_t)i);
                return ERR_BAD_REFERENCE;
            }
            key_seg s = find_key_segment(rt->param_value[i],
                                         &keys_pool[axis_keys_begin[a]],
                                         axis_keys_cnt[a]);
            rt->axis_idx[a] = s.idx;
            rt->axis_weight[a] = s.weight;
        }
    }

    /* ---- C2c: binding combos (combo builder) ---- */
    const int32_t *bind_begin = ir.i32(SLOT_BINDING_BEGIN);
    const int32_t *bind_cnt = ir.i32(SLOT_BINDING_COUNT);
    const int32_t *axis_idx_pool = ir.i32(SLOT_AXIS_IDX);
    for (int32_t bi = 0; bi < n_binds; ++bi) {
        int32_t *kf_idx = &rt->bind_keyform_idx[bi * MAX_COMBO];
        float *w = &rt->bind_weights[bi * MAX_COMBO];
        const int32_t axis_cnt = bind_cnt[bi];
        if (axis_cnt < 0 || axis_cnt > 4) {
            err_set(err, ERR_LIMIT_EXCEEDED, SLOT_BINDING_COUNT, 0, (uint32_t)bi);
            return ERR_LIMIT_EXCEEDED;
        }
        int32_t active = 0;
        for (int32_t i = 0; i < axis_cnt; ++i) {
            const int32_t a = axis_idx_pool[bind_begin[bi] + i];
            if (a < 0 || a >= n_axes) {
                err_set(err, ERR_BAD_REFERENCE, SLOT_AXIS_IDX, 0, (uint32_t)bi);
                return ERR_BAD_REFERENCE;
            }
            if (rt->axis_weight[a] != 0.0f) ++active;
        }
        const int32_t blend_count = 1 << active;
        rt->bind_blend_count[bi] = (uint8_t)blend_count;
        for (int32_t j = 0; j < blend_count; ++j) {
            kf_idx[j] = 0;
            w[j] = 1.0f;
        }
        int32_t index_stride = 1, combo_stride = 1;
        for (int32_t i = 0; i < axis_cnt; ++i) {
            const int32_t a = axis_idx_pool[bind_begin[bi] + i];
            const int32_t idx = rt->axis_idx[a];
            const float weight = rt->axis_weight[a];
            const int32_t key_count = axis_keys_cnt[a];
            const int32_t idx_off = idx * index_stride;
            if (weight != 0.0f) {
                const int32_t next_off = (idx + 1) * index_stride;
                const float inv = 1.0f - weight;
                for (int32_t j = 0; j < blend_count; ++j) {
                    if ((j & combo_stride) == 0) {
                        kf_idx[j] += idx_off;
                        w[j] *= inv;
                    } else {
                        kf_idx[j] += next_off;
                        w[j] *= weight;
                    }
                }
                combo_stride *= 2;
            } else {
                for (int32_t j = 0; j < blend_count; ++j) {
                    kf_idx[j] += idx_off;
                }
            }
            index_stride *= key_count;
        }
    }

    /* ---- C4a: warp/rotation keyform blend ---- */
    const int32_t *warp_kf_off = ir.i32(SLOT_WARP_KF_OFF);
    const int32_t *warp_binding = ir.i32(SLOT_WARP_BINDING);
    const int32_t *warp_vc = ir.i32(SLOT_WARP_VERTEX_COUNT);
    const int32_t *warp_row = ir.i32(SLOT_WARP_ROW);
    const int32_t *warp_col = ir.i32(SLOT_WARP_COLUMN);
    const int32_t *warp_kp_off = ir.i32(SLOT_WARP_KEY_POS_OFF);
    const float *warp_key_opacity = ir.f32(SLOT_WARP_KEY_OPACITY);
    const float *kf_pos_pool = ir.f32(SLOT_KF_POS);
    const int32_t *warp_q = ir.i32(SLOT_WARP_QUAD_TRANSFORM);

    uint32_t warp_off = 0;
    for (int32_t i = 0; i < n_warps; ++i) {
        const int32_t vc = warp_vc[i];
        if (vc <= 0 || vc > (int32_t)MAX_WARP_VERTEX) {
            err_set(err, ERR_LIMIT_EXCEEDED, SLOT_WARP_VERTEX_COUNT, 0, (uint32_t)i);
            return ERR_LIMIT_EXCEEDED;
        }
        const int32_t bi = warp_binding[i];
        const int32_t bc = (bi >= 0 && bi < n_binds) ? rt->bind_blend_count[bi] : 1;
        const int32_t *kf_idx = (bi >= 0 && bi < n_binds) ? &rt->bind_keyform_idx[bi * MAX_COMBO] : nullptr;
        const float *w = (bi >= 0 && bi < n_binds) ? &rt->bind_weights[bi * MAX_COMBO] : nullptr;

        float *dst = &rt->warp_pos[warp_off * 2];
        blend_keyform_positions(kf_pos_pool, warp_kp_off, warp_kf_off[i],
                                kf_idx, w, bc, vc, dst);
        rt->warp_opacity[i] = blend_keyform_scalar(warp_key_opacity, warp_kf_off[i],
                                                   kf_idx, w, bc);
        warp_off += (uint32_t)vc;
    }

    const int32_t *rot_kf_off = ir.i32(SLOT_ROT_KF_OFF);
    const int32_t *rot_binding = ir.i32(SLOT_ROT_BINDING);
    const float *rot_k_opacity = ir.f32(SLOT_ROT_KEY_OPACITY);
    const float *rot_k_angle = ir.f32(SLOT_ROT_KEY_ANGLE);
    const float *rot_k_ox = ir.f32(SLOT_ROT_KEY_ORIGIN_X);
    const float *rot_k_oy = ir.f32(SLOT_ROT_KEY_ORIGIN_Y);
    const float *rot_k_scale = ir.f32(SLOT_ROT_KEY_SCALE);
    const float *rot_base = ir.f32(SLOT_ROT_BASE_ANGLE);

    for (int32_t i = 0; i < n_rots; ++i) {
        const int32_t bi = rot_binding[i];
        const int32_t bc = (bi >= 0 && bi < n_binds) ? rt->bind_blend_count[bi] : 1;
        const int32_t *kf_idx = (bi >= 0 && bi < n_binds) ? &rt->bind_keyform_idx[bi * MAX_COMBO] : nullptr;
        const float *w = (bi >= 0 && bi < n_binds) ? &rt->bind_weights[bi * MAX_COMBO] : nullptr;
        const int32_t off = rot_kf_off[i];
        rt->rot_opacity[i] = blend_keyform_scalar(rot_k_opacity, off, kf_idx, w, bc);
        rt->rot_angle[i] = rot_base[i] + blend_keyform_scalar(rot_k_angle, off, kf_idx, w, bc);
        rt->rot_origin_x[i] = blend_keyform_scalar(rot_k_ox, off, kf_idx, w, bc);
        rt->rot_origin_y[i] = blend_keyform_scalar(rot_k_oy, off, kf_idx, w, bc);
        rt->rot_scale[i] = blend_keyform_scalar(rot_k_scale, off, kf_idx, w, bc);
    }

    /* ---- C4b: deformer topo order (nested chains, depth <= 16) ---- */
    const int32_t *def_type = ir.i32(SLOT_DEF_TYPE);
    const int32_t *def_local = ir.i32(SLOT_DEF_LOCAL);
    const int32_t *def_parent_def = ir.i32(SLOT_DEF_PARENT_DEF);
    if (n_defs > (int32_t)MAX_DEFORMERS || n_warps > (int32_t)MAX_WARPS) {
        err_set(err, ERR_LIMIT_EXCEEDED, SLOT_DEF_PARENT_DEF, 0, 0);
        return ERR_LIMIT_EXCEEDED;
    }

    int32_t depth[MAX_DEFORMERS];
    for (int32_t i = 0; i < n_defs; ++i) {
        int32_t d = 0, cur = i, hops = 0;
        while (def_parent_def[cur] != -1) {
            cur = def_parent_def[cur];
            if (cur < 0 || cur >= n_defs || ++hops > 16) {
                err_set(err, ERR_DEPTH_EXCEEDED, SLOT_DEF_PARENT_DEF, 0, (uint32_t)i);
                return ERR_DEPTH_EXCEEDED;
            }
            ++d;
        }
        depth[i] = d;
    }

    /* process in ascending depth (parent before child) */
    int32_t order[MAX_DEFORMERS];
    for (int32_t i = 0; i < n_defs; ++i) order[i] = i;
    for (int32_t i = 0; i < n_defs; ++i) {
        for (int32_t j = i + 1; j < n_defs; ++j) {
            if (depth[order[j]] < depth[order[i]]) {
                int32_t t = order[i]; order[i] = order[j]; order[j] = t;
            }
        }
    }

    /* accumulated parent scale/opacity for each deformer */
    float def_scale_accum[MAX_DEFORMERS];
    float def_opacity_accum[MAX_DEFORMERS];
    for (int32_t i = 0; i < n_defs; ++i) {
        def_scale_accum[i] = 1.0f;
        def_opacity_accum[i] = 1.0f;
    }

    /* warp grid vertex prefix sums */
    uint32_t warp_base[MAX_WARPS];
    {
        uint32_t acc = 0;
        for (int32_t i = 0; i < n_warps; ++i) {
            warp_base[i] = acc;
            if (warp_vc[i] > 0) acc += (uint32_t)warp_vc[i];
        }
    }

    /* single-level parent transform helper (parent is already composed) */
    auto parent_transform_point = [&](int32_t pi, float x, float y,
                                      float *ox, float *oy) {
        const int32_t ptype = def_type[pi];
        const int32_t plocal = def_local[pi];
        if (ptype == 0 && plocal >= 0 && plocal < n_warps) {
            const int32_t prow = warp_row[plocal], pcol = warp_col[plocal];
            const bool pquad = warp_q != nullptr && warp_q[plocal] != 0;
            const float *pgrid = &rt->warp_pos[warp_base[plocal] * 2];
            float tmp[2] = {x, y};
            warp_transform_point(pgrid, prow, pcol, tmp[0], tmp[1], pquad, tmp);
            *ox = tmp[0];
            *oy = tmp[1];
        } else if (ptype == 1 && plocal >= 0 && plocal < n_rots) {
            float tmp[2];
            rot_transform_point(rt->rot_angle[plocal], rt->rot_scale[plocal],
                                rt->rot_origin_x[plocal], rt->rot_origin_y[plocal],
                                x, y, tmp);
            *ox = tmp[0];
            *oy = tmp[1];
        } else {
            *ox = x;
            *oy = y;
        }
    };

    for (int32_t oi = 0; oi < n_defs; ++oi) {
        const int32_t i = order[oi];
        const int32_t pi = def_parent_def[i];
        const int32_t local = def_local[i];
        if (def_type[i] == 0) { /* WARP */
            if (local < 0 || local >= n_warps) {
                err_set(err, ERR_BAD_REFERENCE, SLOT_DEF_LOCAL, 0, (uint32_t)i);
                return ERR_BAD_REFERENCE;
            }
            float *dst = &rt->warp_pos[warp_base[local] * 2];
            if (pi != -1) {
                const int32_t vc = warp_vc[local];
                for (int32_t v = 0; v < vc; ++v) {
                    float x = dst[v * 2 + 0], y = dst[v * 2 + 1];
                    parent_transform_point(pi, x, y, &dst[v * 2], &dst[v * 2 + 1]);
                }
                def_scale_accum[i] = def_scale_accum[pi];
                def_opacity_accum[i] = rt->warp_opacity[local] * def_opacity_accum[pi];
            } else {
                def_scale_accum[i] = 1.0f;
                def_opacity_accum[i] = rt->warp_opacity[local];
            }
        } else { /* ROTATION */
            if (local < 0 || local >= n_rots) {
                err_set(err, ERR_BAD_REFERENCE, SLOT_DEF_LOCAL, 0, (uint32_t)i);
                return ERR_BAD_REFERENCE;
            }
            float ox = rt->rot_origin_x[local], oy = rt->rot_origin_y[local];
            float ang = rt->rot_angle[local];
            float sc = rt->rot_scale[local];
            float opa = rt->rot_opacity[local];
            if (pi != -1) {
                /* origin through parent */
                float wx = 0.0f, wy = 0.0f;
                parent_transform_point(pi, ox, oy, &wx, &wy);
                /* parent angle probe (numeric, both refs agree) */
                const float dir_delta = (def_type[pi] == 1) ? -10.0f : -0.1f;
                float step = 1.0f;
                float dx = 0.0f, dy = 0.0f;
                for (int iter = 0; iter < 10; ++iter) {
                    float tx = 0.0f, ty = 0.0f;
                    parent_transform_point(pi, ox, oy + step * dir_delta, &tx, &ty);
                    float ddx = tx - wx, ddy = ty - wy;
                    if (ddx != 0.0f || ddy != 0.0f) {
                        dx = ddx; dy = ddy;
                        break;
                    }
                    parent_transform_point(pi, ox, oy - step * dir_delta, &tx, &ty);
                    ddx = tx - wx; ddy = ty - wy;
                    if (ddx != 0.0f || ddy != 0.0f) {
                        dx = -ddx; dy = -ddy;
                        break;
                    }
                    step *= 0.1f;
                }
                /* angle between base dir (0, dir_delta) and direction */
                const float base_ang = atan2f(dir_delta, 0.0f);
                const float dir_ang = atan2f(dy, dx);
                float adj = dir_ang - base_ang;
                while (adj > 3.14159265358979323846f) adj -= 6.28318530717958647692f;
                while (adj < -3.14159265358979323846f) adj += 6.28318530717958647692f;
                ang += adj * 180.0f / 3.14159265358979323846f;
                ox = wx;
                oy = wy;
                sc *= def_scale_accum[pi];
                opa *= def_opacity_accum[pi];
            }
            rt->rot_origin_x[local] = ox;
            rt->rot_origin_y[local] = oy;
            rt->rot_angle[local] = ang;
            rt->rot_scale[local] = sc;
            rt->rot_opacity[local] = opa;
            def_scale_accum[i] = sc;
            def_opacity_accum[i] = opa;
        }
    }

    /* ---- C4c: art_mesh final vertices ---- */
    const int32_t *am_kf_off = ir.i32(SLOT_AM_KF_OFF);
    const int32_t *am_binding = ir.i32(SLOT_AM_BINDING);
    const int32_t *am_vc = ir.i32(SLOT_AM_VERTEX_COUNT);
    const int32_t *am_parent_def = ir.i32(SLOT_AM_PARENT_DEF);
    const int32_t *am_kp_off = ir.i32(SLOT_AM_KEY_POS_OFF);
    const float *am_k_opacity = ir.f32(SLOT_AM_KEY_OPACITY);
    const float *am_k_draw_order = ir.f32(SLOT_AM_KEY_DRAW_ORDER);

    uint32_t am_off = 0;
    for (int32_t i = 0; i < n_ams; ++i) {
        const int32_t vc = am_vc[i];
        if (vc <= 0) {
            rt->mesh_opacity[i] = 0.0f;
            rt->mesh_draw_order[i] = 0.0f;
            continue;
        }
        const int32_t bi = am_binding[i];
        const int32_t bc = (bi >= 0 && bi < n_binds) ? rt->bind_blend_count[bi] : 1;
        const int32_t *kf_idx = (bi >= 0 && bi < n_binds) ? &rt->bind_keyform_idx[bi * MAX_COMBO] : nullptr;
        const float *w = (bi >= 0 && bi < n_binds) ? &rt->bind_weights[bi * MAX_COMBO] : nullptr;

        float *dst = &rt->mesh_pos[am_off * 2];
        blend_keyform_positions(kf_pos_pool, am_kp_off, am_kf_off[i],
                                kf_idx, w, bc, vc, dst);
        rt->mesh_opacity[i] = blend_keyform_scalar(am_k_opacity, am_kf_off[i],
                                                   kf_idx, w, bc);
        rt->mesh_draw_order[i] = blend_keyform_scalar(am_k_draw_order, am_kf_off[i],
                                                      kf_idx, w, bc);

        /* parent deformer transform (parent output is canvas space; one hop) */
        const int32_t pdi = am_parent_def[i];
        if (pdi >= 0 && pdi < n_defs) {
            for (int32_t v = 0; v < vc; ++v) {
                const float x = dst[v * 2 + 0], y = dst[v * 2 + 1];
                parent_transform_point(pdi, x, y, &dst[v * 2], &dst[v * 2 + 1]);
            }
        }
        am_off += (uint32_t)vc;
    }

    return ERR_OK;
}

} // namespace otool::cubism::core
