/**
 * @file moc3_validate.hpp
 * @brief otool_cubism_tool — self Core：C0 校验接口声明（自研）
 */

#pragma once

#include "moc3_common.hpp"
#include "moc3_reader.hpp"

#include <cstdint>
#include <cstddef>

namespace otool::cubism::core {

/**
 * @brief C0 检查：header + offset 表 + CountInfo + CanvasInfo
 *
 * @param data   moc3 blob（不可变，生命周期由调用方保证）
 * @param size   blob 大小
 * @param out    输出模型信息（可空，用于只查错误时）
 * @param err    错误上下文（可空）
 * @return ERR_OK 或错误码（spec/error_codes.md）
 */
err_code moc3_inspect(const uint8_t *data, size_t size, model_info_t *out, err_info *err);

} // namespace otool::cubism::core
