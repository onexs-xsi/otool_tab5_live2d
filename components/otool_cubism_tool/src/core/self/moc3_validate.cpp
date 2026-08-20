/**
 * @file moc3_validate.cpp
 * @brief otool_cubism_tool — self Core：C0 层校验（自研）
 *
 * C0 职责（可行性报告 §6.3）：
 *   magic / version / endian / offset 表 / CountInfo（版本化 word count）/ CanvasInfo
 *   可观察输出：版本、canvas、拒绝原因、memory plan（plan 在 C1 实现）
 *
 * 实现依据 docs/research_log.md 第一轮结论，独立编写：
 *   - 头部 64 字节；offset 表 160/480 个 u32
 *   - 全部 section 8 字节对齐；offset ≤ 文件大小；非零 offset 单调（MVP 要求）
 *   - CountInfo 按版本读取 word count（23/32/35/39），未知长度拒绝
 *   - MVP 仅支持 version=5（profile v5）与 LE
 */

#include "moc3_common.hpp"
#include "moc3_reader.hpp"

namespace otool::cubism::core {
namespace {

constexpr uint32_t SEC_GENERAL = 0xFFFF; /* 通用错误 section */

} // namespace

/**
 * @brief C0 检查：header + offset 表 + CountInfo + CanvasInfo
 *
 * @param data   moc3 blob（不可变）
 * @param size   blob 大小
 * @param out    输出模型信息
 * @param err    错误上下文（可空）
 * @return ERR_OK 或错误码
 */
err_code moc3_inspect(const uint8_t *data, size_t size, model_info_t *out, err_info *err)
{
    err_clear(err);

    if (data == nullptr || size == 0) {
        err_set(err, ERR_NULL_POINTER, SEC_GENERAL, 0);
        return ERR_NULL_POINTER;
    }
    if (size < MOC3_HEADER_SIZE) {
        err_set(err, ERR_TRUNCATED, SEC_GENERAL, (uint32_t)size);
        return ERR_TRUNCATED;
    }
    if (out == nullptr) {
        return ERR_NULL_POINTER;
    }

    byte_reader rd(data, size);

    /* ---- magic ---- */
    uint32_t magic = 0;
    if (!rd.read_u32(0, &magic) || magic != MOC3_MAGIC) {
        err_set(err, ERR_BAD_MAGIC, SEC_GENERAL, 0);
        return ERR_BAD_MAGIC;
    }

    /* ---- version / endian ---- */
    uint8_t ver_byte = 0, endian_flag = 0;
    rd.read_u8(4, &ver_byte);
    rd.read_u8(5, &endian_flag);
    if (ver_byte < 1 || ver_byte > 6) {
        err_set(err, ERR_UNSUPPORTED_VERSION, SEC_GENERAL, 4);
        return ERR_UNSUPPORTED_VERSION;
    }
    if (endian_flag != MOC3_ENDIAN_LITTLE && endian_flag != MOC3_ENDIAN_BIG) {
        err_set(err, ERR_UNKNOWN_FLAG, SEC_GENERAL, 5);
        return ERR_UNKNOWN_FLAG;
    }

    const moc3_version ver = (moc3_version)ver_byte;

    /* MVP profile 门禁：仅版本字节 5（G-FMT 冻结前先收紧，其余明确拒绝） */
    if (ver != moc3_version::v5_0) {
        err_set(err, ERR_PROFILE_MISMATCH, SEC_GENERAL, 4);
        return ERR_PROFILE_MISMATCH;
    }

    /* MVP 仅支持 LE（研究日志：endian_flag 0=LE）；BE 明确拒绝而非静默转换 */
    if (endian_flag != MOC3_ENDIAN_LITTLE) {
        err_set(err, ERR_UNSUPPORTED_ENDIAN, SEC_GENERAL, 5);
        return ERR_UNSUPPORTED_ENDIAN;
    }
    rd.set_swap(false);

    /* ---- offset 表 ---- */
    const uint32_t sec_count = offset_count_for(ver);
    const uint32_t table_bytes = sec_count * 4;
    if (!rd.range_ok(MOC3_OFFSET_TABLE_AT, table_bytes)) {
        err_set(err, ERR_TRUNCATED, SEC_GENERAL, MOC3_OFFSET_TABLE_AT + table_bytes);
        return ERR_TRUNCATED;
    }

    uint32_t offs[MOC3_OFFSETS_V6] = {0};
    uint32_t used = 0;
    uint32_t prev = MOC3_HEADER_SIZE; /* 非零 offset 单调下限 */
    for (uint32_t i = 0; i < sec_count; ++i) {
        uint32_t o = 0;
        if (!rd.read_u32(MOC3_OFFSET_TABLE_AT + i * 4, &o)) {
            err_set(err, ERR_TRUNCATED, (uint16_t)i, MOC3_OFFSET_TABLE_AT + i * 4);
            return ERR_TRUNCATED;
        }
        offs[i] = o;
        if (o == 0) {
            continue; /* 未使用槽位 */
        }
        ++used;
        /* 8 字节对齐（研究日志：全部 section 8 字节对齐） */
        if ((o & 7) != 0) {
            err_set(err, ERR_BAD_ALIGNMENT, (uint16_t)i, o);
            return ERR_BAD_ALIGNMENT;
        }
        if (o > size) {
            err_set(err, ERR_TRUNCATED, (uint16_t)i, o);
            return ERR_TRUNCATED;
        }
        /* MVP 要求非零 offset 单调（PurismCore 实证 + 防重叠） */
        if (o < prev) {
            err_set(err, ERR_SECTION_OVERLAP, (uint16_t)i, o);
            return ERR_SECTION_OVERLAP;
        }
        prev = o;
    }

    /* ---- CountInfo（版本化 word count） ---- */
    const uint32_t ci_words = count_info_words(ver);
    if (ci_words == 0 || ci_words > MOC3_COUNT_INFO_WORDS_MAX) {
        err_set(err, ERR_UNSUPPORTED_VERSION, SEC_GENERAL, 4);
        return ERR_UNSUPPORTED_VERSION;
    }
    if (offs[0] == 0 || (offs[0] & 3) != 0) {
        err_set(err, ERR_BAD_ALIGNMENT, 0, offs[0]);
        return ERR_BAD_ALIGNMENT;
    }
    const uint32_t ci_bytes = ci_words * 4;
    if (!rd.range_ok(offs[0], ci_bytes)) {
        err_set(err, ERR_TRUNCATED, 0, offs[0] + ci_bytes);
        return ERR_TRUNCATED;
    }

    count_info_t ci = {};
    for (uint32_t i = 0; i < ci_words; ++i) {
        int32_t v = 0;
        if (!rd.read_i32(offs[0] + i * 4, &v)) {
            err_set(err, ERR_TRUNCATED, 0, offs[0] + i * 4);
            return ERR_TRUNCATED;
        }
        ci.v[i] = v;
    }
    /* 版本之外的字段强制为 0（防止越界读残留） */
    for (uint32_t i = ci_words; i < CI_COUNT; ++i) {
        ci.v[i] = 0;
    }

    /* 基本计数校验（难点 6：全字段校验，不能只查部分） */
    for (uint32_t i = 0; i < CI_COUNT; ++i) {
        if (ci.v[i] < 0) {
            err_set(err, ERR_CARDINALITY, 0, offs[0] + i * 4);
            return ERR_CARDINALITY;
        }
    }
    /* 形变计数一致性：warps + rotations == deformers */
    if (ci.v[CI_WARPS] + ci.v[CI_ROTATIONS] != ci.v[CI_DEFORMERS]) {
        err_set(err, ERR_CARDINALITY, 0, offs[0] + CI_DEFORMERS * 4);
        return ERR_CARDINALITY;
    }

    /* ---- CanvasInfo ---- */
    if (offs[1] == 0) {
        err_set(err, ERR_TRUNCATED, 1, 0);
        return ERR_TRUNCATED;
    }
    if (!rd.range_ok_aligned(offs[1], 21, 4)) { /* 5×f32 + flag u8 */
        err_set(err, ERR_TRUNCATED, 1, offs[1]);
        return ERR_TRUNCATED;
    }
    float pix = 0, ox = 0, oy = 0, cw = 0, ch = 0;
    uint8_t cflag = 0;
    if (!rd.read_f32(offs[1], &pix) || !rd.read_f32(offs[1] + 4, &ox) ||
        !rd.read_f32(offs[1] + 8, &oy) || !rd.read_f32(offs[1] + 12, &cw) ||
        !rd.read_f32(offs[1] + 16, &ch) || !rd.read_u8(offs[1] + 20, &cflag)) {
        err_set(err, ERR_TRUNCATED, 1, offs[1]);
        return ERR_TRUNCATED;
    }
    /* 拒绝 NaN/Inf 画布（§6.2 最低校验集） */
    if (!(pix > 0.0f) || !(cw > 0.0f) || !(ch > 0.0f)) {
        err_set(err, ERR_NAN_OR_INF, 1, offs[1]);
        return ERR_NAN_OR_INF;
    }

    /* ---- 输出 ---- */
    out->version = ver;
    out->endian_flag = endian_flag;
    out->file_size = (uint32_t)size;
    out->pix_per_unit = pix;
    out->origin_x = ox;
    out->origin_y = oy;
    out->canvas_width = cw;
    out->canvas_height = ch;
    out->canvas_flag = cflag;
    out->counts = ci;
    out->section_count = used;

    return ERR_OK;
}

} // namespace otool::cubism::core
