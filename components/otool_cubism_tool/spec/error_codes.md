# spec — 错误码体系（草案）

> 状态：draft。定义 `ot_core_error_t` 的分类与携带信息；
> 解析/更新失败必须给出稳定 error code + section + byte offset。

## 1. 错误码分类（草案）

```c
typedef enum ot_core_error {
    OT_CORE_OK                        = 0,

    /* ---- 输入/参数 ---- */
    OT_CORE_ERR_NULL_POINTER          = 0x1001,
    OT_CORE_ERR_INVALID_SIZE          = 0x1002,
    OT_CORE_ERR_BAD_ALIGNMENT         = 0x1003,

    /* ---- header / profile ---- */
    OT_CORE_ERR_BAD_MAGIC             = 0x2001,
    OT_CORE_ERR_UNSUPPORTED_VERSION   = 0x2002,
    OT_CORE_ERR_UNSUPPORTED_ENDIAN    = 0x2003,
    OT_CORE_ERR_UNKNOWN_FLAG          = 0x2004,
    OT_CORE_ERR_PROFILE_MISMATCH      = 0x2005,

    /* ---- 结构完整性 ---- */
    OT_CORE_ERR_TRUNCATED             = 0x3001,   /* 超出 blob 边界 */
    OT_CORE_ERR_OFFSET_OVERFLOW       = 0x3002,   /* offset+count*size 溢出 */
    OT_CORE_ERR_SECTION_OVERLAP       = 0x3003,
    OT_CORE_ERR_BAD_REFERENCE         = 0x3004,   /* index/parent/mask/binding 越界 */
    OT_CORE_ERR_CYCLE                 = 0x3005,   /* deformer/part 依赖环 */
    OT_CORE_ERR_DEPTH_EXCEEDED        = 0x3006,
    OT_CORE_ERR_CARDINALITY           = 0x3007,   /* 计数关系不一致 */

    /* ---- 数值 ---- */
    OT_CORE_ERR_NAN_OR_INF            = 0x4001,
    OT_CORE_ERR_KEY_NOT_MONOTONIC     = 0x4002,
    OT_CORE_ERR_DUPLICATE_KEY         = 0x4003,

    /* ---- 资源 ---- */
    OT_CORE_ERR_LIMIT_EXCEEDED        = 0x5001,   /* hard limit（spec/hard_limits.md） */
    OT_CORE_ERR_BUDGET_EXCEEDED       = 0x5002,   /* 内存计划超预算 */
    OT_CORE_ERR_ARENA_TOO_SMALL       = 0x5003,
    OT_CORE_ERR_NO_MEMORY             = 0x5004,

    /* ---- 状态机 ---- */
    OT_CORE_ERR_INVALID_STATE         = 0x6001,
    OT_CORE_ERR_NOT_IMPLEMENTED       = 0x6002,   /* 已知但未实现的 feature */
} ot_core_error_t;
```

## 2. 错误上下文

所有解析/更新错误通过以下结构返回稳定上下文：

```c
typedef struct ot_core_error_info {
    ot_core_error_t code;
    uint16_t section_id;     /* 文件 section/表标识 */
    uint32_t byte_offset;    /* 出错处相对 blob 起始的偏移 */
    uint32_t index;          /* 出错元素序号（如适用） */
} ot_core_error_info_t;
```

- `code` 必须稳定：同一输入同一错误不得因优化或实现细节改变。
- `section_id` 分配表在 format 规格冻结时定义（0 = 通用，1 = header，2 = CountInfo，…）。
- 错误路径不得触发内存分配（使用调用者提供的 out 参数）。
