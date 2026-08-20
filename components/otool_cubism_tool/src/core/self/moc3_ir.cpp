/**
 * @file moc3_ir.cpp
 * @brief otool_cubism_tool — self Core：C1 不可变 IR 解析与校验（自研）
 *
 * 152 项 section 规格表（v5：V30 101 + V33 1 + V42 35 + V50 15），
 * 解析全部 typed 视图并执行引用完整性校验。
 * 实现为自研，依据 docs/research_log.md 的布局结论独立编写。
 */

#include "moc3_ir.hpp"
#include "moc3_validate.hpp"

#include <cstring>

namespace otool::cubism::core {
namespace {

constexpr uint16_t SEC_GENERAL = 0xFFFF; /* 通用错误 section */

/* 版本化 section 规格：slot、元素大小、CountInfo 计数域（0xFFFF=静态 1） */
struct sec_spec {
    uint16_t slot;
    uint8_t elem_size;
    uint16_t count_field;   /* count_info 字段下标；0xFFFF = 静态 1 */
};

#define SEC(slot_, elem_, count_field_) \
    { (uint16_t)(slot_), (uint8_t)(elem_), (uint16_t)(count_field_) }
#define SEC1(slot_, elem_) \
    { (uint16_t)(slot_), (uint8_t)(elem_), (uint16_t)0xFFFF }

/* v5 全部 152 个 section（按 offset 表下标顺序） */
const sec_spec V5_SPECS[] = {
    SEC1(SLOT_COUNT_INFO, ELEM_I32), SEC1(SLOT_CANVAS_INFO, ELEM_F32),
    SEC(SLOT_PART_ID_RUNTIME, ELEM_PTR_SLOT, CI_PARTS),
    SEC(SLOT_PART_ID, ELEM_ID, CI_PARTS),
    SEC(SLOT_PART_BINDING, ELEM_I32, CI_PARTS),
    SEC(SLOT_PART_KF_OFF, ELEM_I32, CI_PARTS),
    SEC(SLOT_PART_KF_COUNT, ELEM_I32, CI_PARTS),
    SEC(SLOT_PART_VISIBLE, ELEM_I32, CI_PARTS),
    SEC(SLOT_PART_ENABLE, ELEM_I32, CI_PARTS),
    SEC(SLOT_PART_PARENT, ELEM_I32, CI_PARTS),
    SEC(SLOT_DEF_ID_RUNTIME, ELEM_PTR_SLOT, CI_DEFORMERS),
    SEC(SLOT_DEF_ID, ELEM_ID, CI_DEFORMERS),
    SEC(SLOT_DEF_BINDING, ELEM_I32, CI_DEFORMERS),
    SEC(SLOT_DEF_VISIBLE, ELEM_I32, CI_DEFORMERS),
    SEC(SLOT_DEF_ENABLE, ELEM_I32, CI_DEFORMERS),
    SEC(SLOT_DEF_PARENT_PART, ELEM_I32, CI_DEFORMERS),
    SEC(SLOT_DEF_PARENT_DEF, ELEM_I32, CI_DEFORMERS),
    SEC(SLOT_DEF_TYPE, ELEM_I32, CI_DEFORMERS),
    SEC(SLOT_DEF_LOCAL, ELEM_I32, CI_DEFORMERS),
    SEC(SLOT_WARP_BINDING, ELEM_I32, CI_WARPS),
    SEC(SLOT_WARP_KF_OFF, ELEM_I32, CI_WARPS),
    SEC(SLOT_WARP_KF_COUNT, ELEM_I32, CI_WARPS),
    SEC(SLOT_WARP_VERTEX_COUNT, ELEM_I32, CI_WARPS),
    SEC(SLOT_WARP_ROW, ELEM_I32, CI_WARPS),
    SEC(SLOT_WARP_COLUMN, ELEM_I32, CI_WARPS),
    SEC(SLOT_ROT_BINDING, ELEM_I32, CI_ROTATIONS),
    SEC(SLOT_ROT_KF_OFF, ELEM_I32, CI_ROTATIONS),
    SEC(SLOT_ROT_KF_COUNT, ELEM_I32, CI_ROTATIONS),
    SEC(SLOT_ROT_BASE_ANGLE, ELEM_F32, CI_ROTATIONS),
    SEC(SLOT_AM_ID_RUNTIME, ELEM_PTR_SLOT, CI_ART_MESHES),
    SEC(SLOT_AM_UV_RUNTIME, ELEM_PTR_SLOT, CI_ART_MESHES),
    SEC(SLOT_AM_IDX_RUNTIME, ELEM_PTR_SLOT, CI_ART_MESHES),
    SEC(SLOT_AM_MASK_RUNTIME, ELEM_PTR_SLOT, CI_ART_MESHES),
    SEC(SLOT_AM_ID, ELEM_ID, CI_ART_MESHES),
    SEC(SLOT_AM_BINDING, ELEM_I32, CI_ART_MESHES),
    SEC(SLOT_AM_KF_OFF, ELEM_I32, CI_ART_MESHES),
    SEC(SLOT_AM_KF_COUNT, ELEM_I32, CI_ART_MESHES),
    SEC(SLOT_AM_VISIBLE, ELEM_I32, CI_ART_MESHES),
    SEC(SLOT_AM_ENABLE, ELEM_I32, CI_ART_MESHES),
    SEC(SLOT_AM_PARENT_PART, ELEM_I32, CI_ART_MESHES),
    SEC(SLOT_AM_PARENT_DEF, ELEM_I32, CI_ART_MESHES),
    SEC(SLOT_AM_TEXTURE_NO, ELEM_I32, CI_ART_MESHES),
    SEC(SLOT_AM_DRAWABLE_FLAG, ELEM_U8, CI_ART_MESHES),
    SEC(SLOT_AM_VERTEX_COUNT, ELEM_I32, CI_ART_MESHES),
    SEC(SLOT_AM_UV_BEGIN, ELEM_I32, CI_ART_MESHES),
    SEC(SLOT_AM_IDX_BEGIN, ELEM_I32, CI_ART_MESHES),
    SEC(SLOT_AM_IDX_COUNT, ELEM_I32, CI_ART_MESHES),
    SEC(SLOT_AM_MASK_BEGIN, ELEM_I32, CI_ART_MESHES),
    SEC(SLOT_AM_MASK_COUNT, ELEM_I32, CI_ART_MESHES),
    SEC(SLOT_PARAM_ID_RUNTIME, ELEM_PTR_SLOT, CI_PARAMETERS),
    SEC(SLOT_PARAM_ID, ELEM_ID, CI_PARAMETERS),
    SEC(SLOT_PARAM_MAX, ELEM_F32, CI_PARAMETERS),
    SEC(SLOT_PARAM_MIN, ELEM_F32, CI_PARAMETERS),
    SEC(SLOT_PARAM_DEFAULT, ELEM_F32, CI_PARAMETERS),
    SEC(SLOT_PARAM_REPEAT, ELEM_I32, CI_PARAMETERS),
    SEC(SLOT_PARAM_DECIMAL, ELEM_I32, CI_PARAMETERS),
    SEC(SLOT_PARAM_AXIS_BEGIN, ELEM_I32, CI_PARAMETERS),
    SEC(SLOT_PARAM_AXIS_COUNT, ELEM_I32, CI_PARAMETERS),
    SEC(SLOT_PART_KEY_DRAW_ORDER, ELEM_F32, CI_PART_KF),
    SEC(SLOT_WARP_KEY_OPACITY, ELEM_F32, CI_WARP_KF),
    SEC(SLOT_WARP_KEY_POS_OFF, ELEM_I32, CI_WARP_KF),
    SEC(SLOT_ROT_KEY_OPACITY, ELEM_F32, CI_ROTATION_KF),
    SEC(SLOT_ROT_KEY_ANGLE, ELEM_F32, CI_ROTATION_KF),
    SEC(SLOT_ROT_KEY_ORIGIN_X, ELEM_F32, CI_ROTATION_KF),
    SEC(SLOT_ROT_KEY_ORIGIN_Y, ELEM_F32, CI_ROTATION_KF),
    SEC(SLOT_ROT_KEY_SCALE, ELEM_F32, CI_ROTATION_KF),
    SEC(SLOT_ROT_KEY_REFLECT_X, ELEM_I32, CI_ROTATION_KF),
    SEC(SLOT_ROT_KEY_REFLECT_Y, ELEM_I32, CI_ROTATION_KF),
    SEC(SLOT_AM_KEY_OPACITY, ELEM_F32, CI_ART_MESH_KF),
    SEC(SLOT_AM_KEY_DRAW_ORDER, ELEM_F32, CI_ART_MESH_KF),
    SEC(SLOT_AM_KEY_POS_OFF, ELEM_I32, CI_ART_MESH_KF),
    SEC(SLOT_KF_POS, ELEM_F32, CI_KF_POS),
    SEC(SLOT_AXIS_IDX, ELEM_I32, CI_AXIS_INDICES),
    SEC(SLOT_BINDING_BEGIN, ELEM_I32, CI_BINDINGS),
    SEC(SLOT_BINDING_COUNT, ELEM_I32, CI_BINDINGS),
    SEC(SLOT_AXIS_KEYS_BEGIN, ELEM_I32, CI_AXES),
    SEC(SLOT_AXIS_KEYS_COUNT, ELEM_I32, CI_AXES),
    SEC(SLOT_KEYS, ELEM_F32, CI_KEYS),
    SEC(SLOT_UV, ELEM_F32, CI_UVS),
    SEC(SLOT_INDICES, ELEM_U16, CI_INDICES),
    SEC(SLOT_MASK, ELEM_I32, CI_MASKS),
    SEC(SLOT_DG_OBJ_BEGIN, ELEM_I32, CI_DRAW_GROUPS),
    SEC(SLOT_DG_OBJ_COUNT, ELEM_I32, CI_DRAW_GROUPS),
    SEC(SLOT_DG_OBJ_TOTAL, ELEM_I32, CI_DRAW_GROUPS),
    SEC(SLOT_DG_MAX_ORDER, ELEM_I32, CI_DRAW_GROUPS),
    SEC(SLOT_DG_MIN_ORDER, ELEM_I32, CI_DRAW_GROUPS),
    SEC(SLOT_DGO_TYPE, ELEM_I32, CI_DRAW_ITEMS),
    SEC(SLOT_DGO_INDEX, ELEM_I32, CI_DRAW_ITEMS),
    SEC(SLOT_DGO_SELF_GROUP, ELEM_I32, CI_DRAW_ITEMS),
    SEC(SLOT_GLUE_ID_RUNTIME, ELEM_PTR_SLOT, CI_GLUES),
    SEC(SLOT_GLUE_ID, ELEM_ID, CI_GLUES),
    SEC(SLOT_GLUE_BINDING, ELEM_I32, CI_GLUES),
    SEC(SLOT_GLUE_KF_OFF, ELEM_I32, CI_GLUES),
    SEC(SLOT_GLUE_KF_COUNT, ELEM_I32, CI_GLUES),
    SEC(SLOT_GLUE_AM_A, ELEM_I32, CI_GLUES),
    SEC(SLOT_GLUE_AM_B, ELEM_I32, CI_GLUES),
    SEC(SLOT_GLUE_INFO_BEGIN, ELEM_I32, CI_GLUES),
    SEC(SLOT_GLUE_INFO_COUNT, ELEM_I32, CI_GLUES),
    SEC(SLOT_GLUE_INFO_WEIGHT, ELEM_F32, CI_GLUE_INFO),
    SEC(SLOT_GLUE_INFO_POS_IDX, ELEM_U16, CI_GLUE_INFO),
    SEC(SLOT_GLUE_KEY_INTENSITY, ELEM_F32, CI_GLUE_KF),
    /* V33 */
    SEC(SLOT_WARP_QUAD_TRANSFORM, ELEM_I32, CI_WARPS),
    /* V42 */
    SEC(SLOT_PARAM_KEY_RUNTIME, ELEM_PTR_SLOT, CI_PARAMETERS),
    SEC(SLOT_PARAM_KEYS_BEGIN, ELEM_I32, CI_PARAMETERS),
    SEC(SLOT_PARAM_KEYS_COUNT, ELEM_I32, CI_PARAMETERS),
    SEC(SLOT_WARP_KEY_COLOR_OFF, ELEM_I32, CI_WARPS),
    SEC(SLOT_ROT_KEY_COLOR_OFF, ELEM_I32, CI_ROTATIONS),
    SEC(SLOT_AM_KEY_COLOR_OFF, ELEM_I32, CI_ART_MESHES),
    SEC(SLOT_KF_MUL_R, ELEM_F32, CI_KF_MUL_COLORS),
    SEC(SLOT_KF_MUL_G, ELEM_F32, CI_KF_MUL_COLORS),
    SEC(SLOT_KF_MUL_B, ELEM_F32, CI_KF_MUL_COLORS),
    SEC(SLOT_KF_SCR_R, ELEM_F32, CI_KF_SCR_COLORS),
    SEC(SLOT_KF_SCR_G, ELEM_F32, CI_KF_SCR_COLORS),
    SEC(SLOT_KF_SCR_B, ELEM_F32, CI_KF_SCR_COLORS),
    SEC(SLOT_PARAM_TYPE, ELEM_I32, CI_PARAMETERS),
    SEC(SLOT_PARAM_BS_BEGIN, ELEM_I32, CI_PARAMETERS),
    SEC(SLOT_PARAM_BS_COUNT, ELEM_I32, CI_PARAMETERS),
    SEC(SLOT_BS_AXIS_KEYS_BEGIN, ELEM_I32, CI_BLEND_AXES),
    SEC(SLOT_BS_AXIS_KEYS_COUNT, ELEM_I32, CI_BLEND_AXES),
    SEC(SLOT_BS_AXIS_BASE_KEY, ELEM_I32, CI_BLEND_AXES),
    SEC(SLOT_BS_BINDING_AXIS, ELEM_I32, CI_BLEND_BINDINGS),
    SEC(SLOT_BS_BINDING_BS_BEGIN, ELEM_I32, CI_BLEND_BINDINGS),
    SEC(SLOT_BS_BINDING_BS_COUNT, ELEM_I32, CI_BLEND_BINDINGS),
    SEC(SLOT_BS_BINDING_CI_BEGIN, ELEM_I32, CI_BLEND_BINDINGS),
    SEC(SLOT_BS_BINDING_CI_COUNT, ELEM_I32, CI_BLEND_BINDINGS),
    SEC(SLOT_BS_WARP_TARGET, ELEM_I32, CI_BS_WARPS),
    SEC(SLOT_BS_WARP_BIND_BEGIN, ELEM_I32, CI_BS_WARPS),
    SEC(SLOT_BS_WARP_BIND_COUNT, ELEM_I32, CI_BS_WARPS),
    SEC(SLOT_BS_AM_TARGET, ELEM_I32, CI_BS_ART_MESHES),
    SEC(SLOT_BS_AM_BIND_BEGIN, ELEM_I32, CI_BS_ART_MESHES),
    SEC(SLOT_BS_AM_BIND_COUNT, ELEM_I32, CI_BS_ART_MESHES),
    SEC(SLOT_BS_CI_CONSTRAINT, ELEM_I32, CI_BS_CONSTRAINT_IDX),
    SEC(SLOT_BS_CONSTRAINT_PARAM, ELEM_I32, CI_BS_CONSTRAINTS),
    SEC(SLOT_BS_CONSTRAINT_VAL_BEGIN, ELEM_I32, CI_BS_CONSTRAINTS),
    SEC(SLOT_BS_CONSTRAINT_VAL_COUNT, ELEM_I32, CI_BS_CONSTRAINTS),
    SEC(SLOT_BS_CONSTRAINT_VAL_KEY, ELEM_F32, CI_BS_CONSTRAINT_VALS),
    SEC(SLOT_BS_CONSTRAINT_VAL_WEIGHT, ELEM_F32, CI_BS_CONSTRAINT_VALS),
    /* V50 */
    SEC(SLOT_WARP_KEY_KMCO, ELEM_I32, CI_WARP_KF),
    SEC(SLOT_WARP_KEY_KSCO, ELEM_I32, CI_WARP_KF),
    SEC(SLOT_ROT_KEY_KMCO, ELEM_I32, CI_ROTATION_KF),
    SEC(SLOT_ROT_KEY_KSCO, ELEM_I32, CI_ROTATION_KF),
    SEC(SLOT_AM_KEY_KMCO, ELEM_I32, CI_ART_MESH_KF),
    SEC(SLOT_AM_KEY_KSCO, ELEM_I32, CI_ART_MESH_KF),
    SEC(SLOT_BS_PART_TARGET, ELEM_I32, CI_BS_PARTS),
    SEC(SLOT_BS_PART_BIND_BEGIN, ELEM_I32, CI_BS_PARTS),
    SEC(SLOT_BS_PART_BIND_COUNT, ELEM_I32, CI_BS_PARTS),
    SEC(SLOT_BS_ROT_TARGET, ELEM_I32, CI_BS_ROTATIONS),
    SEC(SLOT_BS_ROT_BIND_BEGIN, ELEM_I32, CI_BS_ROTATIONS),
    SEC(SLOT_BS_ROT_BIND_COUNT, ELEM_I32, CI_BS_ROTATIONS),
    SEC(SLOT_BS_GLUE_TARGET, ELEM_I32, CI_BS_GLUES),
    SEC(SLOT_BS_GLUE_BIND_BEGIN, ELEM_I32, CI_BS_GLUES),
    SEC(SLOT_BS_GLUE_BIND_COUNT, ELEM_I32, CI_BS_GLUES),
};

constexpr uint32_t V5_SPEC_COUNT = sizeof(V5_SPECS) / sizeof(V5_SPECS[0]);
static_assert(V5_SPEC_COUNT == MOC3_SLOTS_V5, "v5 spec table must have 152 entries");

/* ------------------------------------------------------------------ */
/* 校验辅助                                                            */
/* ------------------------------------------------------------------ */

struct ir_builder {
    const byte_reader *rd;
    const count_info_t *ci;
    const uint32_t *offsets;
    moc3_ir *ir;
    err_info *err;

    bool fail(err_code c, uint16_t sec, uint32_t off, uint32_t idx = 0) const
    {
        err_set(err, c, sec, off, idx);
        return false;
    }

    /* 范围校验：begin..begin+count 在 [0, max) 内（count==0 允许 begin 任意） */
    bool range_ok(int32_t begin, int32_t count, int32_t max) const
    {
        if (count < 0) {
            return false;
        }
        if (count == 0) {
            return true;
        }
        return begin >= 0 && (uint32_t)begin + (uint32_t)count <= (uint32_t)max;
    }

    bool idx_ok(int32_t v, int32_t max) const { return v >= 0 && v < max; }
    bool idx_ok_or_neg1(int32_t v, int32_t max) const { return v >= -1 && v < max; }

    /* keyform 组合校验：keyform_offset + 2^axis_count ≤ keyform 总数 */
    bool key_combo_ok(int32_t kf_off, int32_t binding_idx, int32_t kf_total,
                      uint16_t sec, uint32_t idx) const
    {
        if (binding_idx < 0 || binding_idx >= ci->v[CI_BINDINGS]) {
            return fail(ERR_BAD_REFERENCE, sec, 0, idx);
        }
        const int32_t *bind_count = (const int32_t *)ir->slots[SLOT_BINDING_COUNT].ptr;
        if (bind_count == nullptr) {
            return fail(ERR_TRUNCATED, sec, 0, idx);
        }
        int32_t axis_cnt = bind_count[binding_idx];
        if (axis_cnt < 0 || axis_cnt > 16) {
            return fail(ERR_CARDINALITY, sec, 0, idx);
        }
        int32_t max_blend = 1 << axis_cnt;
        if (kf_off < 0 || (uint32_t)kf_off + (uint32_t)max_blend > (uint32_t)kf_total) {
            return fail(ERR_BAD_REFERENCE, sec, 0, idx);
        }
        return true;
    }
};

/* ------------------------------------------------------------------ */
/* 引用完整性校验（C1 静态校验全集）                                     */
/* ------------------------------------------------------------------ */

static bool verify_static(ir_builder &b)
{
    const count_info_t &ci = *b.ci;
    moc3_ir &ir = *b.ir;
    const int32_t n_parts = ci.v[CI_PARTS];
    const int32_t n_defs = ci.v[CI_DEFORMERS];
    const int32_t n_warps = ci.v[CI_WARPS];
    const int32_t n_rots = ci.v[CI_ROTATIONS];
    const int32_t n_ams = ci.v[CI_ART_MESHES];
    const int32_t n_params = ci.v[CI_PARAMETERS];
    const int32_t n_bind = ci.v[CI_BINDINGS];
    const int32_t n_axes = ci.v[CI_AXES];
    const int32_t n_keys = ci.v[CI_KEYS];
    const int32_t n_kf_pos = ci.v[CI_KF_POS];
    const int32_t n_uvs = ci.v[CI_UVS];
    const int32_t n_indices = ci.v[CI_INDICES];
    const int32_t n_masks = ci.v[CI_MASKS];

    const int32_t *part_binding = ir.i32(SLOT_PART_BINDING);
    const int32_t *part_kf_off = ir.i32(SLOT_PART_KF_OFF);
    const int32_t *part_kf_cnt = ir.i32(SLOT_PART_KF_COUNT);
    const int32_t *part_parent = ir.i32(SLOT_PART_PARENT);
    const int32_t *def_binding = ir.i32(SLOT_DEF_BINDING);
    const int32_t *def_parent_part = ir.i32(SLOT_DEF_PARENT_PART);
    const int32_t *def_parent_def = ir.i32(SLOT_DEF_PARENT_DEF);
    const int32_t *def_type = ir.i32(SLOT_DEF_TYPE);
    const int32_t *def_local = ir.i32(SLOT_DEF_LOCAL);
    const int32_t *warp_binding = ir.i32(SLOT_WARP_BINDING);
    const int32_t *warp_kf_off = ir.i32(SLOT_WARP_KF_OFF);
    const int32_t *warp_kf_cnt = ir.i32(SLOT_WARP_KF_COUNT);
    const int32_t *warp_vc = ir.i32(SLOT_WARP_VERTEX_COUNT);
    const int32_t *warp_row = ir.i32(SLOT_WARP_ROW);
    const int32_t *warp_col = ir.i32(SLOT_WARP_COLUMN);
    const int32_t *rot_binding = ir.i32(SLOT_ROT_BINDING);
    const int32_t *rot_kf_off = ir.i32(SLOT_ROT_KF_OFF);
    const int32_t *rot_kf_cnt = ir.i32(SLOT_ROT_KF_COUNT);
    const int32_t *am_binding = ir.i32(SLOT_AM_BINDING);
    const int32_t *am_kf_off = ir.i32(SLOT_AM_KF_OFF);
    const int32_t *am_kf_cnt = ir.i32(SLOT_AM_KF_COUNT);
    const int32_t *am_parent_part = ir.i32(SLOT_AM_PARENT_PART);
    const int32_t *am_parent_def = ir.i32(SLOT_AM_PARENT_DEF);
    const int32_t *am_vc = ir.i32(SLOT_AM_VERTEX_COUNT);
    const int32_t *am_uv_begin = ir.i32(SLOT_AM_UV_BEGIN);
    const int32_t *am_idx_begin = ir.i32(SLOT_AM_IDX_BEGIN);
    const int32_t *am_idx_cnt = ir.i32(SLOT_AM_IDX_COUNT);
    const int32_t *am_mask_begin = ir.i32(SLOT_AM_MASK_BEGIN);
    const int32_t *am_mask_cnt = ir.i32(SLOT_AM_MASK_COUNT);
    const int32_t *param_axis_begin = ir.i32(SLOT_PARAM_AXIS_BEGIN);
    const int32_t *param_axis_cnt = ir.i32(SLOT_PARAM_AXIS_COUNT);
    const int32_t *bind_begin = ir.i32(SLOT_BINDING_BEGIN);
    const int32_t *bind_cnt = ir.i32(SLOT_BINDING_COUNT);
    const int32_t *axis_idx = ir.i32(SLOT_AXIS_IDX);
    const int32_t *axis_keys_begin = ir.i32(SLOT_AXIS_KEYS_BEGIN);
    const int32_t *axis_keys_cnt = ir.i32(SLOT_AXIS_KEYS_COUNT);
    const int32_t *warp_key_pos_off = ir.i32(SLOT_WARP_KEY_POS_OFF);
    const int32_t *am_key_pos_off = ir.i32(SLOT_AM_KEY_POS_OFF);
    const int32_t *mask_am = ir.i32(SLOT_MASK);
    const int32_t *glue_binding = ir.i32(SLOT_GLUE_BINDING);
    const int32_t *glue_kf_off = ir.i32(SLOT_GLUE_KF_OFF);
    const int32_t *glue_kf_cnt = ir.i32(SLOT_GLUE_KF_COUNT);
    const int32_t *glue_am_a = ir.i32(SLOT_GLUE_AM_A);
    const int32_t *glue_am_b = ir.i32(SLOT_GLUE_AM_B);
    const int32_t *glue_info_begin = ir.i32(SLOT_GLUE_INFO_BEGIN);
    const int32_t *glue_info_cnt = ir.i32(SLOT_GLUE_INFO_COUNT);
    const int32_t *dgo_type = ir.i32(SLOT_DGO_TYPE);
    const int32_t *dgo_index = ir.i32(SLOT_DGO_INDEX);
    const int32_t *dgo_self = ir.i32(SLOT_DGO_SELF_GROUP);
    const uint16_t *glue_pos_idx = ir.u16(SLOT_GLUE_INFO_POS_IDX);
    const int32_t *am_vc_for_glue = am_vc;

    /* parts */
    for (int32_t i = 0; i < n_parts; ++i) {
        if (!b.idx_ok(part_binding[i], n_bind) ||
            !b.range_ok(part_kf_off[i], part_kf_cnt[i], ci.v[CI_PART_KF]) ||
            !b.idx_ok_or_neg1(part_parent[i], n_parts) ||
            !b.key_combo_ok(part_kf_off[i], part_binding[i], ci.v[CI_PART_KF],
                            SLOT_PART_KF_OFF, (uint32_t)i)) {
            return b.fail(ERR_BAD_REFERENCE, SLOT_PART_BINDING, 0, (uint32_t)i);
        }
    }

    /* deformers */
    for (int32_t i = 0; i < n_defs; ++i) {
        if (!b.idx_ok(def_binding[i], n_bind) ||
            !b.idx_ok_or_neg1(def_parent_part[i], n_parts) ||
            !b.idx_ok_or_neg1(def_parent_def[i], n_defs)) {
            return b.fail(ERR_BAD_REFERENCE, SLOT_DEF_BINDING, 0, (uint32_t)i);
        }
        if (def_type[i] == 0) { /* WARP */
            if (!b.idx_ok(def_local[i], n_warps)) {
                return b.fail(ERR_BAD_REFERENCE, SLOT_DEF_LOCAL, 0, (uint32_t)i);
            }
        } else if (def_type[i] == 1) { /* ROTATION */
            if (!b.idx_ok(def_local[i], n_rots)) {
                return b.fail(ERR_BAD_REFERENCE, SLOT_DEF_LOCAL, 0, (uint32_t)i);
            }
        } else {
            return b.fail(ERR_UNKNOWN_FLAG, SLOT_DEF_TYPE, 0, (uint32_t)i);
        }
    }

    /* warps */
    for (int32_t i = 0; i < n_warps; ++i) {
        if (!b.idx_ok(warp_binding[i], n_bind) ||
            !b.range_ok(warp_kf_off[i], warp_kf_cnt[i], ci.v[CI_WARP_KF])) {
            return b.fail(ERR_BAD_REFERENCE, SLOT_WARP_BINDING, 0, (uint32_t)i);
        }
        if (warp_row[i] <= 0 || warp_col[i] <= 0 ||
            warp_vc[i] != (warp_row[i] + 1) * (warp_col[i] + 1)) {
            return b.fail(ERR_CARDINALITY, SLOT_WARP_VERTEX_COUNT, 0, (uint32_t)i);
        }
        if (!b.key_combo_ok(warp_kf_off[i], warp_binding[i], ci.v[CI_WARP_KF],
                            SLOT_WARP_KF_OFF, (uint32_t)i)) {
            return b.fail(ERR_BAD_REFERENCE, SLOT_WARP_KF_OFF, 0, (uint32_t)i);
        }
        /* keyform 位置池引用 */
        for (int32_t j = 0; j < warp_kf_cnt[i]; ++j) {
            int32_t po = warp_key_pos_off[warp_kf_off[i] + j];
            if (!b.range_ok(po, warp_vc[i], n_kf_pos)) {
                return b.fail(ERR_BAD_REFERENCE, SLOT_WARP_KEY_POS_OFF, 0,
                              (uint32_t)(warp_kf_off[i] + j));
            }
        }
    }

    /* rotations */
    for (int32_t i = 0; i < n_rots; ++i) {
        if (!b.idx_ok(rot_binding[i], n_bind) ||
            !b.range_ok(rot_kf_off[i], rot_kf_cnt[i], ci.v[CI_ROTATION_KF])) {
            return b.fail(ERR_BAD_REFERENCE, SLOT_ROT_BINDING, 0, (uint32_t)i);
        }
        if (!b.key_combo_ok(rot_kf_off[i], rot_binding[i], ci.v[CI_ROTATION_KF],
                            SLOT_ROT_KF_OFF, (uint32_t)i)) {
            return b.fail(ERR_BAD_REFERENCE, SLOT_ROT_KF_OFF, 0, (uint32_t)i);
        }
    }

    /* art meshes */
    for (int32_t i = 0; i < n_ams; ++i) {
        if (!b.idx_ok(am_binding[i], n_bind) ||
            !b.range_ok(am_kf_off[i], am_kf_cnt[i], ci.v[CI_ART_MESH_KF]) ||
            !b.idx_ok_or_neg1(am_parent_part[i], n_parts) ||
            !b.idx_ok_or_neg1(am_parent_def[i], n_defs)) {
            return b.fail(ERR_BAD_REFERENCE, SLOT_AM_BINDING, 0, (uint32_t)i);
        }
        if (am_vc[i] < 0 || am_uv_begin[i] < 0 ||
            (uint32_t)am_uv_begin[i] + 2u * (uint32_t)am_vc[i] > (uint32_t)n_uvs ||
            !b.range_ok(am_idx_begin[i], am_idx_cnt[i], n_indices) ||
            !b.range_ok(am_mask_begin[i], am_mask_cnt[i], n_masks)) {
            return b.fail(ERR_BAD_REFERENCE, SLOT_AM_UV_BEGIN, 0, (uint32_t)i);
        }
        if (!b.key_combo_ok(am_kf_off[i], am_binding[i], ci.v[CI_ART_MESH_KF],
                            SLOT_AM_KF_OFF, (uint32_t)i)) {
            return b.fail(ERR_BAD_REFERENCE, SLOT_AM_KF_OFF, 0, (uint32_t)i);
        }
        /* art mesh keyform 位置池引用 */
        for (int32_t j = 0; j < am_kf_cnt[i]; ++j) {
            int32_t po = am_key_pos_off[am_kf_off[i] + j];
            if (!b.idx_ok(po, n_kf_pos)) {
                return b.fail(ERR_BAD_REFERENCE, SLOT_AM_KEY_POS_OFF, 0,
                              (uint32_t)(am_kf_off[i] + j));
            }
        }
        /* 索引池引用 */
        const uint16_t *idx = ir.u16(SLOT_INDICES);
        for (int32_t j = 0; j < am_idx_cnt[i]; ++j) {
            if (idx[am_idx_begin[i] + j] >= (uint16_t)am_vc[i]) {
                return b.fail(ERR_BAD_REFERENCE, SLOT_INDICES, 0,
                              (uint32_t)(am_idx_begin[i] + j));
            }
        }
    }

    /* parameters → axis 范围 */
    for (int32_t i = 0; i < n_params; ++i) {
        if (!b.range_ok(param_axis_begin[i], param_axis_cnt[i], n_axes)) {
            return b.fail(ERR_BAD_REFERENCE, SLOT_PARAM_AXIS_BEGIN, 0, (uint32_t)i);
        }
    }

    /* bindings → axis_idx；axis_idx → axes；axes → keys */
    for (int32_t i = 0; i < n_bind; ++i) {
        if (!b.range_ok(bind_begin[i], bind_cnt[i], ci.v[CI_AXIS_INDICES])) {
            return b.fail(ERR_BAD_REFERENCE, SLOT_BINDING_BEGIN, 0, (uint32_t)i);
        }
    }
    for (int32_t i = 0; i < ci.v[CI_AXIS_INDICES]; ++i) {
        if (!b.idx_ok(axis_idx[i], n_axes)) {
            return b.fail(ERR_BAD_REFERENCE, SLOT_AXIS_IDX, 0, (uint32_t)i);
        }
    }
    for (int32_t i = 0; i < n_axes; ++i) {
        if (!b.range_ok(axis_keys_begin[i], axis_keys_cnt[i], n_keys)) {
            return b.fail(ERR_BAD_REFERENCE, SLOT_AXIS_KEYS_BEGIN, 0, (uint32_t)i);
        }
    }

    /* masks */
    for (int32_t i = 0; i < n_masks; ++i) {
        if (!b.idx_ok_or_neg1(mask_am[i], n_ams)) {
            return b.fail(ERR_BAD_REFERENCE, SLOT_MASK, 0, (uint32_t)i);
        }
    }

    /* glues */
    for (int32_t i = 0; i < ci.v[CI_GLUES]; ++i) {
        if (!b.idx_ok(glue_binding[i], n_bind) ||
            !b.range_ok(glue_kf_off[i], glue_kf_cnt[i], ci.v[CI_GLUE_KF]) ||
            !b.idx_ok(glue_am_a[i], n_ams) || !b.idx_ok(glue_am_b[i], n_ams) ||
            !b.range_ok(glue_info_begin[i], glue_info_cnt[i], ci.v[CI_GLUE_INFO])) {
            return b.fail(ERR_BAD_REFERENCE, SLOT_GLUE_BINDING, 0, (uint32_t)i);
        }
        /* glue position idx 引用两个 art mesh 的顶点 */
        if (glue_pos_idx != nullptr && am_vc_for_glue != nullptr) {
            int32_t vc0 = am_vc_for_glue[glue_am_a[i]];
            int32_t vc1 = am_vc_for_glue[glue_am_b[i]];
            for (int32_t j = 0; j < glue_info_cnt[i]; ++j) {
                uint16_t p = glue_pos_idx[glue_info_begin[i] + j];
                uint16_t lim = (uint16_t)((j & 1) ? vc1 : vc0);
                if (p >= lim) {
                    return b.fail(ERR_BAD_REFERENCE, SLOT_GLUE_INFO_POS_IDX, 0,
                                  (uint32_t)(glue_info_begin[i] + j));
                }
            }
        }
    }

    /* draw group objects */
    for (int32_t i = 0; i < ci.v[CI_DRAW_ITEMS]; ++i) {
        if (!b.idx_ok_or_neg1(dgo_self[i], ci.v[CI_DRAW_GROUPS])) {
            return b.fail(ERR_BAD_REFERENCE, SLOT_DGO_SELF_GROUP, 0, (uint32_t)i);
        }
        if (dgo_type[i] == 0) {
            if (!b.idx_ok_or_neg1(dgo_index[i], n_ams)) {
                return b.fail(ERR_BAD_REFERENCE, SLOT_DGO_INDEX, 0, (uint32_t)i);
            }
        } else if (dgo_type[i] == 1) {
            if (!b.idx_ok_or_neg1(dgo_index[i], n_parts)) {
                return b.fail(ERR_BAD_REFERENCE, SLOT_DGO_INDEX, 0, (uint32_t)i);
            }
        } else {
            return b.fail(ERR_UNKNOWN_FLAG, SLOT_DGO_TYPE, 0, (uint32_t)i);
        }
    }

    /* draw group obj 范围 */
    const int32_t *dg_begin = ir.i32(SLOT_DG_OBJ_BEGIN);
    const int32_t *dg_cnt = ir.i32(SLOT_DG_OBJ_COUNT);
    for (int32_t i = 0; i < ci.v[CI_DRAW_GROUPS]; ++i) {
        if (!b.range_ok(dg_begin[i], dg_cnt[i], ci.v[CI_DRAW_ITEMS])) {
            return b.fail(ERR_BAD_REFERENCE, SLOT_DG_OBJ_BEGIN, 0, (uint32_t)i);
        }
    }

    /* V42+：参数扩展键、keyform 颜色引用 */
    const int32_t *param_keys_begin = ir.i32(SLOT_PARAM_KEYS_BEGIN);
    const int32_t *param_keys_cnt = ir.i32(SLOT_PARAM_KEYS_COUNT);
    if (param_keys_begin != nullptr && param_keys_cnt != nullptr) {
        for (int32_t i = 0; i < n_params; ++i) {
            if (!b.range_ok(param_keys_begin[i], param_keys_cnt[i], n_keys)) {
                return b.fail(ERR_BAD_REFERENCE, SLOT_PARAM_KEYS_BEGIN, 0, (uint32_t)i);
            }
        }
    }
    const int32_t *warp_kco = ir.i32(SLOT_WARP_KEY_COLOR_OFF);
    if (warp_kco != nullptr) {
        for (int32_t i = 0; i < n_warps; ++i) {
            if (!b.range_ok(warp_kco[i], warp_kf_cnt[i], ci.v[CI_KF_MUL_COLORS])) {
                return b.fail(ERR_BAD_REFERENCE, SLOT_WARP_KEY_COLOR_OFF, 0, (uint32_t)i);
            }
        }
    }
    const int32_t *rot_kco = ir.i32(SLOT_ROT_KEY_COLOR_OFF);
    if (rot_kco != nullptr) {
        for (int32_t i = 0; i < n_rots; ++i) {
            if (!b.range_ok(rot_kco[i], rot_kf_cnt[i], ci.v[CI_KF_MUL_COLORS])) {
                return b.fail(ERR_BAD_REFERENCE, SLOT_ROT_KEY_COLOR_OFF, 0, (uint32_t)i);
            }
        }
    }
    const int32_t *am_kco = ir.i32(SLOT_AM_KEY_COLOR_OFF);
    if (am_kco != nullptr) {
        for (int32_t i = 0; i < n_ams; ++i) {
            if (!b.range_ok(am_kco[i], am_kf_cnt[i], ci.v[CI_KF_MUL_COLORS])) {
                return b.fail(ERR_BAD_REFERENCE, SLOT_AM_KEY_COLOR_OFF, 0, (uint32_t)i);
            }
        }
    }

    /* blend shape（V42+）*/
    const int32_t *param_bs_begin = ir.i32(SLOT_PARAM_BS_BEGIN);
    const int32_t *param_bs_cnt = ir.i32(SLOT_PARAM_BS_COUNT);
    if (param_bs_begin != nullptr && param_bs_cnt != nullptr) {
        for (int32_t i = 0; i < n_params; ++i) {
            if (!b.range_ok(param_bs_begin[i], param_bs_cnt[i], ci.v[CI_BLEND_AXES])) {
                return b.fail(ERR_BAD_REFERENCE, SLOT_PARAM_BS_BEGIN, 0, (uint32_t)i);
            }
        }
        const int32_t *bs_axis_begin = ir.i32(SLOT_BS_AXIS_KEYS_BEGIN);
        const int32_t *bs_axis_cnt = ir.i32(SLOT_BS_AXIS_KEYS_COUNT);
        for (int32_t i = 0; i < ci.v[CI_BLEND_AXES]; ++i) {
            if (!b.range_ok(bs_axis_begin[i], bs_axis_cnt[i], n_keys)) {
                return b.fail(ERR_BAD_REFERENCE, SLOT_BS_AXIS_KEYS_BEGIN, 0, (uint32_t)i);
            }
        }
        const int32_t *bs_bind_axis = ir.i32(SLOT_BS_BINDING_AXIS);
        for (int32_t i = 0; i < ci.v[CI_BLEND_BINDINGS]; ++i) {
            if (!b.idx_ok(bs_bind_axis[i], ci.v[CI_BLEND_AXES])) {
                return b.fail(ERR_BAD_REFERENCE, SLOT_BS_BINDING_AXIS, 0, (uint32_t)i);
            }
        }
        const int32_t *bs_warp_target = ir.i32(SLOT_BS_WARP_TARGET);
        for (int32_t i = 0; i < ci.v[CI_BS_WARPS]; ++i) {
            if (!b.idx_ok(bs_warp_target[i], n_warps)) {
                return b.fail(ERR_BAD_REFERENCE, SLOT_BS_WARP_TARGET, 0, (uint32_t)i);
            }
        }
        const int32_t *bs_am_target = ir.i32(SLOT_BS_AM_TARGET);
        for (int32_t i = 0; i < ci.v[CI_BS_ART_MESHES]; ++i) {
            if (!b.idx_ok(bs_am_target[i], n_ams)) {
                return b.fail(ERR_BAD_REFERENCE, SLOT_BS_AM_TARGET, 0, (uint32_t)i);
            }
        }
    }

    /* V50：bs_part/bs_rotation/bs_glue target */
    const int32_t *bs_part_target = ir.i32(SLOT_BS_PART_TARGET);
    if (bs_part_target != nullptr) {
        for (int32_t i = 0; i < ci.v[CI_BS_PARTS]; ++i) {
            if (!b.idx_ok(bs_part_target[i], n_parts)) {
                return b.fail(ERR_BAD_REFERENCE, SLOT_BS_PART_TARGET, 0, (uint32_t)i);
            }
        }
    }
    const int32_t *bs_rot_target = ir.i32(SLOT_BS_ROT_TARGET);
    if (bs_rot_target != nullptr) {
        for (int32_t i = 0; i < ci.v[CI_BS_ROTATIONS]; ++i) {
            if (!b.idx_ok(bs_rot_target[i], n_rots)) {
                return b.fail(ERR_BAD_REFERENCE, SLOT_BS_ROT_TARGET, 0, (uint32_t)i);
            }
        }
    }
    const int32_t *bs_glue_target = ir.i32(SLOT_BS_GLUE_TARGET);
    if (bs_glue_target != nullptr) {
        for (int32_t i = 0; i < ci.v[CI_BS_GLUES]; ++i) {
            if (!b.idx_ok(bs_glue_target[i], ci.v[CI_GLUES])) {
                return b.fail(ERR_BAD_REFERENCE, SLOT_BS_GLUE_TARGET, 0, (uint32_t)i);
            }
        }
    }

    return true;
}

} // namespace

err_code moc3_build_ir(const uint8_t *data, size_t size, moc3_ir *out_ir, err_info *err)
{
    err_clear(err);
    if (data == nullptr || out_ir == nullptr) {
        err_set(err, ERR_NULL_POINTER, SEC_GENERAL, 0);
        return ERR_NULL_POINTER;
    }

    /* C0 前置校验 */
    model_info_t info = {};
    err_code rc = moc3_inspect(data, size, &info, err);
    if (rc != ERR_OK) {
        return rc;
    }
    if (info.version != moc3_version::v5_0) {
        err_set(err, ERR_PROFILE_MISMATCH, SEC_GENERAL, 4);
        return ERR_PROFILE_MISMATCH;
    }

    byte_reader rd(data, size);
    rd.set_swap(false);

    /* 读取 offset 表 */
    uint32_t offs[MOC3_OFFSETS_V6] = {0};
    for (uint32_t i = 0; i < MOC3_SLOTS_V5; ++i) {
        if (!rd.read_u32(MOC3_OFFSET_TABLE_AT + i * 4, &offs[i])) {
            err_set(err, ERR_TRUNCATED, (uint16_t)i, MOC3_OFFSET_TABLE_AT + i * 4);
            return ERR_TRUNCATED;
        }
    }

    /* 解析 152 个 section 视图 */
    moc3_ir ir = {};
    ir.info = info;
    ir.slot_count = MOC3_SLOTS_V5;
    const count_info_t &ci = info.counts;

    uint32_t prev_end = MOC3_HEADER_SIZE; /* 不重叠检查（严格单调） */
    for (uint32_t i = 0; i < MOC3_SLOTS_V5; ++i) {
        const sec_spec &spec = V5_SPECS[i];
        uint32_t count = (spec.count_field == 0xFFFF) ? 1 : (uint32_t)ci.v[spec.count_field];
        sec_view &v = ir.slots[spec.slot];
        v.elem_size = spec.elem_size;
        v.count = count;
        v.offset = offs[i];
        if (offs[i] == 0 || count == 0) {
            v.ptr = nullptr; /* 未使用或空 section */
            continue;
        }
        /* bounds：对齐 8、范围内、单调不重叠 */
        if ((offs[i] & 7) != 0) {
            err_set(err, ERR_BAD_ALIGNMENT, (uint16_t)i, offs[i]);
            return ERR_BAD_ALIGNMENT;
        }
        uint32_t bytes = 0;
        if (!mul_ok(count, spec.elem_size, 0x7FFFFFFFu, &bytes) ||
            !rd.range_ok(offs[i], bytes)) {
            err_set(err, ERR_TRUNCATED, (uint16_t)i, offs[i]);
            return ERR_TRUNCATED;
        }
        if (offs[i] < prev_end) {
            err_set(err, ERR_SECTION_OVERLAP, (uint16_t)i, offs[i]);
            return ERR_SECTION_OVERLAP;
        }
        prev_end = offs[i] + bytes;
        v.ptr = rd.ptr(offs[i]);
    }

    /* 引用完整性校验 */
    ir_builder b = {&rd, &ci, offs, &ir, err};
    if (!verify_static(b)) {
        return err->code;
    }

    *out_ir = ir;
    return ERR_OK;
}

} // namespace otool::cubism::core
