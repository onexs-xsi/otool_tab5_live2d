/**
 * @file moc3_ir.hpp
 * @brief otool_cubism_tool — self Core：C1 不可变模型 IR（自研）
 *
 * C1 职责（可行性报告 §6.3）：把通过 C0 校验的 moc3 blob 解析为
 * 不可变 typed IR——所有 section 的 typed 视图（指向 blob 内，不复制）
 * + 全部静态引用完整性校验（G-FMT 输入）。
 *
 * v5 共 152 个 section（slot 0..151）：V30 101 + V33 1 + V42 35 + V50 15
 * （研究日志第二轮更正：V50 为 15 项，Mao 实测 152 非零吻合）。
 */

#pragma once

#include "moc3_common.hpp"
#include "moc3_reader.hpp"

#include <cstdint>
#include <cstddef>

namespace otool::cubism::core {

/** section 元素尺寸（字节） */
enum sec_elem : uint8_t {
    ELEM_I32 = 4,
    ELEM_F32 = 4,
    ELEM_U8 = 1,
    ELEM_U16 = 2,
    ELEM_ID = 64,        /* ID 字符串 64 字节 */
    ELEM_PTR_SLOT = 8,   /* id_runtime 等指针占位槽（导出器 64 位） */
};

/** 每个 section 的解析后视图 */
struct sec_view {
    const uint8_t *ptr;   /* blob 内指针；count==0 时为 nullptr */
    uint32_t offset;      /* 文件内偏移（0 = 未使用） */
    uint32_t count;       /* 元素数（来自 CountInfo 或 1） */
    uint8_t elem_size;    /* 元素字节数 */
};

/** C1 IR：152 个 section 视图 + 元数据 */
struct moc3_ir {
    model_info_t info;         /* C0 结果 */
    uint32_t slot_count;
    sec_view slots[152];

    /* 便捷访问器 */
    const sec_view &s(uint32_t slot) const { return slots[slot]; }

    const int32_t *i32(uint32_t slot) const { return (const int32_t *)s(slot).ptr; }
    const float *f32(uint32_t slot) const { return (const float *)s(slot).ptr; }
    const uint16_t *u16(uint32_t slot) const { return (const uint16_t *)s(slot).ptr; }
    const uint8_t *u8(uint32_t slot) const { return s(slot).ptr; }
};

/* ---------------- slot 编号（与 offset 表下标一致） ---------------- */
enum slot : uint16_t {
    SLOT_COUNT_INFO = 0,
    SLOT_CANVAS_INFO = 1,
    /* parts */
    SLOT_PART_ID_RUNTIME = 2, SLOT_PART_ID = 3, SLOT_PART_BINDING = 4,
    SLOT_PART_KF_OFF = 5, SLOT_PART_KF_COUNT = 6, SLOT_PART_VISIBLE = 7,
    SLOT_PART_ENABLE = 8, SLOT_PART_PARENT = 9,
    /* deformers */
    SLOT_DEF_ID_RUNTIME = 10, SLOT_DEF_ID = 11, SLOT_DEF_BINDING = 12,
    SLOT_DEF_VISIBLE = 13, SLOT_DEF_ENABLE = 14, SLOT_DEF_PARENT_PART = 15,
    SLOT_DEF_PARENT_DEF = 16, SLOT_DEF_TYPE = 17, SLOT_DEF_LOCAL = 18,
    /* warps */
    SLOT_WARP_BINDING = 19, SLOT_WARP_KF_OFF = 20, SLOT_WARP_KF_COUNT = 21,
    SLOT_WARP_VERTEX_COUNT = 22, SLOT_WARP_ROW = 23, SLOT_WARP_COLUMN = 24,
    /* rotations */
    SLOT_ROT_BINDING = 25, SLOT_ROT_KF_OFF = 26, SLOT_ROT_KF_COUNT = 27,
    SLOT_ROT_BASE_ANGLE = 28,
    /* art meshes */
    SLOT_AM_ID_RUNTIME = 29, SLOT_AM_UV_RUNTIME = 30,
    SLOT_AM_IDX_RUNTIME = 31, SLOT_AM_MASK_RUNTIME = 32,
    SLOT_AM_ID = 33, SLOT_AM_BINDING = 34, SLOT_AM_KF_OFF = 35,
    SLOT_AM_KF_COUNT = 36, SLOT_AM_VISIBLE = 37, SLOT_AM_ENABLE = 38,
    SLOT_AM_PARENT_PART = 39, SLOT_AM_PARENT_DEF = 40,
    SLOT_AM_TEXTURE_NO = 41, SLOT_AM_DRAWABLE_FLAG = 42,
    SLOT_AM_VERTEX_COUNT = 43, SLOT_AM_UV_BEGIN = 44,
    SLOT_AM_IDX_BEGIN = 45, SLOT_AM_IDX_COUNT = 46,
    SLOT_AM_MASK_BEGIN = 47, SLOT_AM_MASK_COUNT = 48,
    /* parameters */
    SLOT_PARAM_ID_RUNTIME = 49, SLOT_PARAM_ID = 50, SLOT_PARAM_MAX = 51,
    SLOT_PARAM_MIN = 52, SLOT_PARAM_DEFAULT = 53, SLOT_PARAM_REPEAT = 54,
    SLOT_PARAM_DECIMAL = 55, SLOT_PARAM_AXIS_BEGIN = 56, SLOT_PARAM_AXIS_COUNT = 57,
    /* keyforms */
    SLOT_PART_KEY_DRAW_ORDER = 58,
    SLOT_WARP_KEY_OPACITY = 59, SLOT_WARP_KEY_POS_OFF = 60,
    SLOT_ROT_KEY_OPACITY = 61, SLOT_ROT_KEY_ANGLE = 62,
    SLOT_ROT_KEY_ORIGIN_X = 63, SLOT_ROT_KEY_ORIGIN_Y = 64,
    SLOT_ROT_KEY_SCALE = 65, SLOT_ROT_KEY_REFLECT_X = 66, SLOT_ROT_KEY_REFLECT_Y = 67,
    SLOT_AM_KEY_OPACITY = 68, SLOT_AM_KEY_DRAW_ORDER = 69, SLOT_AM_KEY_POS_OFF = 70,
    /* shared pools */
    SLOT_KF_POS = 71, SLOT_AXIS_IDX = 72, SLOT_BINDING_BEGIN = 73,
    SLOT_BINDING_COUNT = 74, SLOT_AXIS_KEYS_BEGIN = 75, SLOT_AXIS_KEYS_COUNT = 76,
    SLOT_KEYS = 77, SLOT_UV = 78, SLOT_INDICES = 79, SLOT_MASK = 80,
    /* draw groups */
    SLOT_DG_OBJ_BEGIN = 81, SLOT_DG_OBJ_COUNT = 82, SLOT_DG_OBJ_TOTAL = 83,
    SLOT_DG_MAX_ORDER = 84, SLOT_DG_MIN_ORDER = 85,
    SLOT_DGO_TYPE = 86, SLOT_DGO_INDEX = 87, SLOT_DGO_SELF_GROUP = 88,
    /* glues */
    SLOT_GLUE_ID_RUNTIME = 89, SLOT_GLUE_ID = 90, SLOT_GLUE_BINDING = 91,
    SLOT_GLUE_KF_OFF = 92, SLOT_GLUE_KF_COUNT = 93, SLOT_GLUE_AM_A = 94,
    SLOT_GLUE_AM_B = 95, SLOT_GLUE_INFO_BEGIN = 96, SLOT_GLUE_INFO_COUNT = 97,
    SLOT_GLUE_INFO_WEIGHT = 98, SLOT_GLUE_INFO_POS_IDX = 99,
    SLOT_GLUE_KEY_INTENSITY = 100,
    /* V33 */
    SLOT_WARP_QUAD_TRANSFORM = 101,
    /* V42 */
    SLOT_PARAM_KEY_RUNTIME = 102, SLOT_PARAM_KEYS_BEGIN = 103, SLOT_PARAM_KEYS_COUNT = 104,
    SLOT_WARP_KEY_COLOR_OFF = 105, SLOT_ROT_KEY_COLOR_OFF = 106,
    SLOT_AM_KEY_COLOR_OFF = 107,
    SLOT_KF_MUL_R = 108, SLOT_KF_MUL_G = 109, SLOT_KF_MUL_B = 110,
    SLOT_KF_SCR_R = 111, SLOT_KF_SCR_G = 112, SLOT_KF_SCR_B = 113,
    SLOT_PARAM_TYPE = 114, SLOT_PARAM_BS_BEGIN = 115, SLOT_PARAM_BS_COUNT = 116,
    SLOT_BS_AXIS_KEYS_BEGIN = 117, SLOT_BS_AXIS_KEYS_COUNT = 118,
    SLOT_BS_AXIS_BASE_KEY = 119,
    SLOT_BS_BINDING_AXIS = 120, SLOT_BS_BINDING_BS_BEGIN = 121,
    SLOT_BS_BINDING_BS_COUNT = 122, SLOT_BS_BINDING_CI_BEGIN = 123,
    SLOT_BS_BINDING_CI_COUNT = 124,
    SLOT_BS_WARP_TARGET = 125, SLOT_BS_WARP_BIND_BEGIN = 126, SLOT_BS_WARP_BIND_COUNT = 127,
    SLOT_BS_AM_TARGET = 128, SLOT_BS_AM_BIND_BEGIN = 129, SLOT_BS_AM_BIND_COUNT = 130,
    SLOT_BS_CI_CONSTRAINT = 131,
    SLOT_BS_CONSTRAINT_PARAM = 132, SLOT_BS_CONSTRAINT_VAL_BEGIN = 133,
    SLOT_BS_CONSTRAINT_VAL_COUNT = 134,
    SLOT_BS_CONSTRAINT_VAL_KEY = 135, SLOT_BS_CONSTRAINT_VAL_WEIGHT = 136,
    /* V50 */
    SLOT_WARP_KEY_KMCO = 137, SLOT_WARP_KEY_KSCO = 138,
    SLOT_ROT_KEY_KMCO = 139, SLOT_ROT_KEY_KSCO = 140,
    SLOT_AM_KEY_KMCO = 141, SLOT_AM_KEY_KSCO = 142,
    SLOT_BS_PART_TARGET = 143, SLOT_BS_PART_BIND_BEGIN = 144, SLOT_BS_PART_BIND_COUNT = 145,
    SLOT_BS_ROT_TARGET = 146, SLOT_BS_ROT_BIND_BEGIN = 147, SLOT_BS_ROT_BIND_COUNT = 148,
    SLOT_BS_GLUE_TARGET = 149, SLOT_BS_GLUE_BIND_BEGIN = 150, SLOT_BS_GLUE_BIND_COUNT = 151,
    SLOT_LAST = 151,
};

constexpr uint32_t MOC3_SLOTS_V5 = 152;

/**
 * @brief C1：解析并校验 moc3 blob → 不可变 IR
 *
 * 校验覆盖（研究日志第一轮"最低校验集" + PurismCore 行为研究，自研实现）：
 *   - 全部 section bounds（对齐 8 / 范围内 / 单调 / 不重叠）
 *   - CountInfo 一致性（warps+rotations==deformers、非负）
 *   - 引用完整性：part/deformer/art_mesh 父链、binding、axis、keyform 组合、
 *     warp 网格、UV/indices/mask 范围、glue 引用、draw group、blend shape
 *
 * @return ERR_OK 或错误码（err 携带 section/offset）
 */
err_code moc3_build_ir(const uint8_t *data, size_t size, moc3_ir *out_ir, err_info *err);

} // namespace otool::cubism::core
