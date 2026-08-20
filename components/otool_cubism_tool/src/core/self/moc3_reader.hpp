/**
 * @file moc3_reader.hpp
 * @brief otool_cubism_tool — self Core：有界字节读取器（自研）
 *
 * 所有读取都带长度边界与 checked arithmetic（可行性报告 §6.2）：
 *   - 每个 offset+count×size 在解引用前验证，禁止溢出/越界
 *   - 错误必须带稳定 code + section + byte offset（spec/error_codes.md）
 * 受限 C++17，无 STL。
 */

#pragma once

#include "moc3_common.hpp"

#include <cstdint>
#include <cstddef>

namespace otool::cubism::core {

/** 错误码（与 spec/error_codes.md 对应） */
enum err_code : uint32_t {
    ERR_OK = 0,
    ERR_NULL_POINTER = 0x1001,
    ERR_INVALID_SIZE = 0x1002,
    ERR_BAD_ALIGNMENT = 0x1003,
    ERR_BAD_MAGIC = 0x2001,
    ERR_UNSUPPORTED_VERSION = 0x2002,
    ERR_UNSUPPORTED_ENDIAN = 0x2003,
    ERR_UNKNOWN_FLAG = 0x2004,
    ERR_PROFILE_MISMATCH = 0x2005,
    ERR_TRUNCATED = 0x3001,
    ERR_OFFSET_OVERFLOW = 0x3002,
    ERR_SECTION_OVERLAP = 0x3003,
    ERR_BAD_REFERENCE = 0x3004,
    ERR_CYCLE = 0x3005,
    ERR_DEPTH_EXCEEDED = 0x3006,
    ERR_CARDINALITY = 0x3007,
    ERR_NAN_OR_INF = 0x4001,
    ERR_KEY_NOT_MONOTONIC = 0x4002,
    ERR_DUPLICATE_KEY = 0x4003,
    ERR_LIMIT_EXCEEDED = 0x5001,
    ERR_BUDGET_EXCEEDED = 0x5002,
    ERR_ARENA_TOO_SMALL = 0x5003,
    ERR_NO_MEMORY = 0x5004,
    ERR_INVALID_STATE = 0x6001,
    ERR_NOT_IMPLEMENTED = 0x6002,
};

/** 稳定错误上下文（spec/error_codes.md §2） */
struct err_info {
    err_code code;
    uint16_t section_id;     /* 出错 section（offset 表下标或 0xFFFF=通用） */
    uint32_t byte_offset;    /* 相对 blob 起始 */
    uint32_t index;          /* 出错元素序号 */
};

inline void err_clear(err_info *e)
{
    if (e != nullptr) {
        e->code = ERR_OK;
        e->section_id = 0;
        e->byte_offset = 0;
        e->index = 0;
    }
}

inline void err_set(err_info *e, err_code c, uint16_t sec, uint32_t off, uint32_t idx = 0)
{
    if (e != nullptr) {
        e->code = c;
        e->section_id = sec;
        e->byte_offset = off;
        e->index = idx;
    }
}

/** checked 加法：a + b 不溢出且 ≤ cap */
inline bool add_ok(uint32_t a, uint32_t b, uint32_t cap, uint32_t *out)
{
    if (a > cap || b > cap - a) {
        return false;
    }
    *out = a + b;
    return true;
}

/** checked 乘法：a * b 不溢出且 ≤ cap */
inline bool mul_ok(uint32_t a, uint32_t b, uint32_t cap, uint32_t *out)
{
    if (a != 0 && b > cap / a) {
        return false;
    }
    *out = a * b;
    return true;
}

/**
 * @brief 有界读取器：对 blob 的只读访问，全部带边界检查
 *
 * swap=true 时多字节字段按大端解释（endian_flag=1）。
 * MVP 仅支持 LE（endian_flag=0），BE 由调用方在 header 层拒绝。
 */
class byte_reader {
public:
    byte_reader(const uint8_t *data, size_t size)
        : data_(data), size_(size), swap_(false)
    {
    }

    size_t size() const { return size_; }
    const uint8_t *base() const { return data_; }

    void set_swap(bool swap) { swap_ = swap; }
    bool swap() const { return swap_; }

    /* 原始范围检查：off..off+len 在 blob 内 */
    bool range_ok(uint32_t off, uint32_t len) const
    {
        return off <= size_ && len <= size_ - off;
    }

    /* 带对齐要求的范围检查 */
    bool range_ok_aligned(uint32_t off, uint32_t len, uint32_t align) const
    {
        return (off & (align - 1)) == 0 && range_ok(off, len);
    }

    bool read_u8(uint32_t off, uint8_t *out) const
    {
        if (!range_ok(off, 1)) {
            return false;
        }
        *out = data_[off];
        return true;
    }

    bool read_u16(uint32_t off, uint16_t *out) const
    {
        if (!range_ok(off, 2)) {
            return false;
        }
        uint16_t v;
        if (swap_) {
            v = (uint16_t)((data_[off] << 8) | data_[off + 1]);
        } else {
            v = (uint16_t)(data_[off] | (data_[off + 1] << 8));
        }
        *out = v;
        return true;
    }

    bool read_u32(uint32_t off, uint32_t *out) const
    {
        if (!range_ok(off, 4)) {
            return false;
        }
        uint32_t v;
        if (swap_) {
            v = ((uint32_t)data_[off] << 24) | ((uint32_t)data_[off + 1] << 16) |
                ((uint32_t)data_[off + 2] << 8) | (uint32_t)data_[off + 3];
        } else {
            v = (uint32_t)data_[off] | ((uint32_t)data_[off + 1] << 8) |
                ((uint32_t)data_[off + 2] << 16) | ((uint32_t)data_[off + 3] << 24);
        }
        *out = v;
        return true;
    }

    bool read_i32(uint32_t off, int32_t *out) const
    {
        uint32_t v;
        if (!read_u32(off, &v)) {
            return false;
        }
        *out = (int32_t)v;
        return true;
    }

    bool read_f32(uint32_t off, float *out) const
    {
        uint32_t v;
        if (!read_u32(off, &v)) {
            return false;
        }
        *out = bit_cast_float(v);
        return true;
    }

    /* 取一段字节的指针（调用方保证长度已校验） */
    const uint8_t *ptr(uint32_t off) const
    {
        return data_ + off;
    }

private:
    static float bit_cast_float(uint32_t v)
    {
        union {
            uint32_t u;
            float f;
        } c;
        c.u = v;
        return c.f;
    }

    const uint8_t *data_;
    size_t size_;
    bool swap_;
};

} // namespace otool::cubism::core
