/**
 * @file moc3_common.hpp
 * @brief otool_cubism_tool — self Core：MOC3 公共常量与类型（自研）
 *
 * 依据 docs/research_log.md 第一轮逆向结论：
 *   - 文件头 64 字节：magic "MOC3" + version u8 + endian_flag u8 + 保留
 *   - offset 表：v1~v5 = 160 个 u32；v6 = 480 个 u32（@ 0x40）
 *   - CountInfo 版本化：v1~v3=23 words, v4=32, v5=35, v6=39
 *   - 全部 section 8 字节对齐、offset ≤ 文件大小、非零 offset 单调
 *
 * 受限 C++17：-fno-exceptions -fno-rtti，无 STL 容器。
 */

#pragma once

#include <cstdint>
#include <cstddef>

namespace otool::cubism::core {

/* ------------------------------------------------------------------ */
/* 版本与布局常量                                                       */
/* ------------------------------------------------------------------ */

enum class moc3_version : uint8_t {
    v3_0 = 1,   /* Cubism 3.0.00 ~ 3.2.07 */
    v3_3 = 2,   /* Cubism 3.3.00 ~ 3.3.03 */
    v4_0 = 3,   /* Cubism 4.0.00 ~ 4.1.05 */
    v4_2 = 4,   /* Cubism 4.2.00 ~ 4.2.04 */
    v5_0 = 5,   /* Cubism 5.0.00 ~ 5.2.03 —— 首选生产 profile */
    v5_3 = 6,   /* Cubism 5.3.00+ —— 单独 Gate */
};

constexpr uint32_t MOC3_MAGIC = 0x33434F4Du;      /* "MOC3" (LE) */
constexpr uint32_t MOC3_HEADER_SIZE = 64;          /* 文件头 */
constexpr uint32_t MOC3_OFFSET_TABLE_AT = 0x40;    /* offset 表起始（= header 大小） */
constexpr uint32_t MOC3_OFFSETS_V5 = 160;          /* v1~v5 */
constexpr uint32_t MOC3_OFFSETS_V6 = 480;          /* v6 */
constexpr uint32_t MOC3_COUNT_INFO_WORDS_MAX = 39; /* v6 上限（含 offscreen 3 项 + 保留） */

/* endian_flag 值 */
constexpr uint8_t MOC3_ENDIAN_LITTLE = 0;
constexpr uint8_t MOC3_ENDIAN_BIG = 1;

/* ------------------------------------------------------------------ */
/* CountInfo：版本化字段表                                              */
/* ------------------------------------------------------------------ */

enum count_field : uint8_t {
    CI_PARTS = 0,
    CI_DEFORMERS,
    CI_WARPS,
    CI_ROTATIONS,
    CI_ART_MESHES,
    CI_PARAMETERS,
    CI_PART_KF,
    CI_WARP_KF,
    CI_ROTATION_KF,
    CI_ART_MESH_KF,
    CI_KF_POS,
    CI_AXIS_INDICES,
    CI_BINDINGS,
    CI_AXES,
    CI_KEYS,
    CI_UVS,
    CI_INDICES,
    CI_MASKS,
    CI_DRAW_GROUPS,
    CI_DRAW_ITEMS,
    CI_GLUES,
    CI_GLUE_INFO,
    CI_GLUE_KF,
    CI_KF_MUL_COLORS,
    CI_KF_SCR_COLORS,
    CI_BLEND_AXES,
    CI_BLEND_BINDINGS,
    CI_BS_WARPS,
    CI_BS_ART_MESHES,
    CI_BS_CONSTRAINT_IDX,
    CI_BS_CONSTRAINTS,
    CI_BS_CONSTRAINT_VALS,
    CI_BS_PARTS,
    CI_BS_ROTATIONS,
    CI_BS_GLUES,
    CI_OFFSCREENS,       /* v6 起 */
    CI_OFFSCREEN_KF,     /* v6 起 */
    CI_BS_OFFSCREENS,    /* v6 起 */
    CI_RESERVED,         /* v6 起 */
    CI_COUNT,
};

/** 各版本 CountInfo 的 word 数（研究日志难点 2） */
constexpr uint32_t count_info_words(moc3_version ver)
{
    switch (ver) {
    case moc3_version::v3_0:
    case moc3_version::v3_3:
        return 23;
    case moc3_version::v4_0:
        return 23; /* v4.0 与 v3 相同 */
    case moc3_version::v4_2:
        return 32;
    case moc3_version::v5_0:
        return 35;
    case moc3_version::v5_3:
        return 39;
    }
    return 0;
}

/** 各版本 offset 表大小（槽位数） */
constexpr uint32_t offset_count_for(moc3_version ver)
{
    return ver == moc3_version::v5_3 ? MOC3_OFFSETS_V6 : MOC3_OFFSETS_V5;
}

/** 解析后的 CountInfo（35 字段全量，v6 字段允许为 0） */
struct count_info_t {
    int32_t v[CI_COUNT];
};

/* ------------------------------------------------------------------ */
/* 输出：inspect 结果（C0 层）                                          */
/* ------------------------------------------------------------------ */

struct model_info_t {
    moc3_version version;
    uint8_t endian_flag;
    uint32_t file_size;

    /* CanvasInfo */
    float pix_per_unit;
    float origin_x;
    float origin_y;
    float canvas_width;
    float canvas_height;
    uint8_t canvas_flag;

    count_info_t counts;
    uint32_t section_count;  /* 实际使用的非零 offset 数 */
};

} // namespace otool::cubism::core
