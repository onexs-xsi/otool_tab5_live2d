/**
 * @file moc3_update.hpp
 * @brief otool_cubism_tool — self Core：C2+C4 最小 update（自研）
 *
 * 计算链（可行性报告 §4.7 实时路径的 Core 部分）：
 *   参数 clamp → 轴 key search（idx/weight）→ binding 组合（2^N keyform+权重）
 *     → 对象 keyform 混合（warp 网格顶点 / rotation 参数 / art_mesh 局部顶点）
 *     → deformer 变换（warp 双线性/三角形插值、rotation 仿射）→ 最终画布顶点
 *
 * MVP 简化（后续轮次补齐）：
 *   - 无嵌套 deformer（Mao 实测 parent_deformer 全 -1；遇到嵌套返回 NOT_IMPLEMENTED）
 *   - 无 repeat 参数（Mao 实测 repeat 全 0；遇到 repeat 返回 NOT_IMPLEMENTED）
 *   - 无 blend shape（Mao 有少量，先忽略）
 *   - warp 外推：clamp 到网格内（边缘顶点可能轻微变形，MVP 可接受）
 */

#pragma once

#include "moc3_ir.hpp"

#include <cstdint>
#include <cstddef>

namespace otool::cubism::core {

/** runtime 工作区（一次分配，update 热路径不分配） */
struct core_runtime {
    const moc3_ir *ir;

    /* 参数解析结果 */
    float *param_value;          /* parameters 个（clamp 后） */
    int32_t *axis_idx;           /* axes 个 */
    float *axis_weight;          /* axes 个 */

    /* binding 组合结果 */
    int32_t *bind_keyform_idx;   /* bindings × 16 */
    float *bind_weights;         /* bindings × 16 */
    uint8_t *bind_blend_count;   /* bindings 个 */

    /* warp 网格顶点（混合后，画布坐标） */
    float *warp_pos;             /* Σ(row+1)(col+1) × 2 */
    /* rotation 参数（混合后） */
    float *rot_angle;            /* rotations 个 */
    float *rot_origin_x;         /* rotations 个 */
    float *rot_origin_y;         /* rotations 个 */
    float *rot_scale;            /* rotations 个 */
    float *rot_opacity;          /* rotations 个 */
    float *warp_opacity;         /* warps 个 */

    /* art_mesh 最终顶点（画布坐标）与不透明度 */
    float *mesh_pos;             /* Σ am_vc × 2 */
    float *mesh_opacity;         /* art_meshes 个 */
    uint32_t *mesh_off;          /* art_meshes+1 前缀和（顶点池偏移） */

    uint32_t total_mesh_vertices; /* Σ am_vc */
    uint32_t total_warp_vertices; /* Σ (row+1)(col+1) */
};

/** 计算 runtime 内存布局大小 */
uint32_t core_runtime_size(const moc3_ir *ir);

/**
 * @brief 分配并初始化 runtime（一次 malloc；free 用 core_runtime_destroy）
 */
err_code core_runtime_create(const moc3_ir *ir, core_runtime **out_rt);

void core_runtime_destroy(core_runtime *rt);

/**
 * @brief 执行一帧 update：参数 → 组合 → keyform 混合 → deformer 变换
 *
 * @param rt     runtime（已 create）
 * @param params 输入参数值（ir 的 parameters 个；可空 = 全用 default）
 * @param err    错误上下文
 */
err_code core_update_frame(core_runtime *rt, const float *params, err_info *err);

} // namespace otool::cubism::core
