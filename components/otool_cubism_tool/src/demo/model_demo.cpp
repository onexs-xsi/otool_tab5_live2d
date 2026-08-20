/**
 * @file model_demo.cpp
 * @brief otool_cubism_tool — 静态模型演示实现（自研）
 *
 * 把嵌入式 moc3 + 纹理素材接入 self Core 管线：
 *   moc3 拷贝到 64B 对齐缓冲（自研 Core 要求对齐，flash 嵌入区不保证）
 *   → moc3_build_ir（C1）→ core_runtime_create（C2 工作区）
 *   → 每帧 core_update_frame（C2+C4）→ prepare_view + render_frame（软光栅）
 */

#include "otool_cubism_demo.h"

#include "moc3_ir.hpp"
#include "moc3_update.hpp"
#include "model_render.hpp"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <cstring>

namespace otool::cubism::demo {

using namespace otool::cubism::core;

namespace {

const char *TAG = "cubism_demo";

struct model_handle_impl {
    uint8_t       *moc3_owned;    /* 原始 malloc 块（内含对齐后的 moc3） */
    uint8_t       *moc3;          /* 64B 对齐后的 moc3 指针 */
    size_t         moc3_size;
    moc3_ir        ir;
    core_runtime  *rt;
    uint16_t      *tex;           /* PSRAM 纹理副本（RGBA4444） */
    uint16_t       tex_w, tex_h;
    float         *params;
    int32_t        n_params;
};

inline model_handle_impl *h2i(model_handle *h)
{
    return reinterpret_cast<model_handle_impl *>(h);
}

inline const model_handle_impl *h2i(const model_handle *h)
{
    return reinterpret_cast<const model_handle_impl *>(h);
}

} // namespace

model_handle *model_create(const model_asset &asset)
{
    if (asset.moc3 == nullptr || asset.moc3_size == 0 ||
        asset.tex == nullptr || asset.tex_w == 0 || asset.tex_h == 0) {
        return nullptr;
    }

    model_handle_impl *h = (model_handle_impl *)heap_caps_malloc(
        sizeof(model_handle_impl), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (h == nullptr) {
        ESP_LOGE(TAG, "no memory for handle");
        return nullptr;
    }
    std::memset(h, 0, sizeof(*h));
    h->tex_w = asset.tex_w;
    h->tex_h = asset.tex_h;

    bool ok = false;
    do {
        /* moc3 → 64B 对齐 PSRAM 副本 */
        const uintptr_t kAlign = 64;
        h->moc3_owned = (uint8_t *)heap_caps_malloc(asset.moc3_size + kAlign,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (h->moc3_owned == nullptr) {
            ESP_LOGE(TAG, "no memory for moc3 copy (%u bytes)", (unsigned)asset.moc3_size);
            break;
        }
        h->moc3 = (uint8_t *)(((uintptr_t)h->moc3_owned + kAlign - 1) & ~(kAlign - 1));
        h->moc3_size = asset.moc3_size;
        std::memcpy(h->moc3, asset.moc3, asset.moc3_size);

        /* C1 IR */
        err_info ir_err = {};
        err_code rc = moc3_build_ir(h->moc3, h->moc3_size, &h->ir, &ir_err);
        if (rc != ERR_OK) {
            ESP_LOGE(TAG, "moc3_build_ir -> 0x%x (sec=%u off=0x%x idx=%u)",
                     (unsigned)rc, (unsigned)ir_err.section_id,
                     (unsigned)ir_err.byte_offset, (unsigned)ir_err.index);
            break;
        }

        /* C2 runtime */
        rc = core_runtime_create(&h->ir, &h->rt);
        if (rc != ERR_OK) {
            ESP_LOGE(TAG, "core_runtime_create -> 0x%x", (unsigned)rc);
            break;
        }

        /* 参数表：默认值 */
        h->n_params = h->ir.info.counts.v[CI_PARAMETERS];
        if (h->n_params > 0) {
            h->params = (float *)heap_caps_malloc((size_t)h->n_params * sizeof(float),
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (h->params == nullptr) {
                ESP_LOGE(TAG, "no memory for params");
                break;
            }
            const float *p_def = h->ir.f32(SLOT_PARAM_DEFAULT);
            for (int32_t i = 0; i < h->n_params; ++i) {
                h->params[i] = p_def[i];
            }
        }

        /* 纹理副本（光栅热路径不读 flash） */
        const size_t tex_bytes = (size_t)asset.tex_w * asset.tex_h * sizeof(uint16_t);
        h->tex = (uint16_t *)heap_caps_malloc(tex_bytes,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (h->tex == nullptr) {
            ESP_LOGE(TAG, "no memory for texture (%u bytes)", (unsigned)tex_bytes);
            break;
        }
        std::memcpy(h->tex, asset.tex, tex_bytes);

        ok = true;
    } while (0);

    if (!ok) {
        model_destroy(reinterpret_cast<model_handle *>(h));
        return nullptr;
    }

    ESP_LOGI(TAG, "model ready: params=%d am=%d warp=%d rot=%d",
             (int)h->ir.info.counts.v[CI_PARAMETERS],
             (int)h->ir.info.counts.v[CI_ART_MESHES],
             (int)h->ir.info.counts.v[CI_WARPS],
             (int)h->ir.info.counts.v[CI_ROTATIONS]);
    return reinterpret_cast<model_handle *>(h);
}

void model_destroy(model_handle *h)
{
    if (h == nullptr) {
        return;
    }
    model_handle_impl *i = h2i(h);
    if (i->rt != nullptr) {
        core_runtime_destroy(i->rt);
    }
    if (i->tex != nullptr) {
        heap_caps_free(i->tex);
    }
    if (i->params != nullptr) {
        heap_caps_free(i->params);
    }
    if (i->moc3_owned != nullptr) {
        heap_caps_free(i->moc3_owned);
    }
    heap_caps_free(i);
}

int model_param_count(const model_handle *h)
{
    return h != nullptr ? (int)h2i(h)->n_params : 0;
}

void model_set_param(model_handle *h, int idx, float value)
{
    if (h == nullptr) {
        return;
    }
    model_handle_impl *i = h2i(h);
    if (idx >= 0 && idx < i->n_params) {
        i->params[idx] = value;
    }
}

bool model_render(model_handle *h, uint16_t *rgb565, uint16_t fb_w, uint16_t fb_h)
{
    if (h == nullptr || rgb565 == nullptr || fb_w == 0 || fb_h == 0) {
        return false;
    }
    model_handle_impl *i = h2i(h);
    if (i->rt == nullptr) {
        return false;
    }

    err_info uerr = {};
    if (core_update_frame(i->rt, i->params, &uerr) != ERR_OK) {
        ESP_LOGE(TAG, "core_update_frame -> 0x%x", (unsigned)uerr.code);
        return false;
    }

    renderer::model_render_input rin = {};
    rin.ir = &i->ir;
    renderer::texture_ref texref = {i->tex, i->tex_w, i->tex_h};
    rin.textures = &texref;
    rin.texture_count = 1;
    renderer::prepare_view(&rin, fb_w, fb_h);
    rin.rt = i->rt;

    renderer::frame_buffer fb = {rgb565, fb_w, fb_h};
    renderer::render_frame(rin, fb, 0x0000);
    return true;
}

} // namespace otool::cubism::demo
