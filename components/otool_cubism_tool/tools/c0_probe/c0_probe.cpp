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

#include <cstdio>
#include <cstdlib>

using namespace otool::cubism::core;

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
    if (argc < 2) {
        std::printf("usage: c0_probe <file.moc3>\n");
        return 2;
    }

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

    std::free(buf);
    return 0;
}
