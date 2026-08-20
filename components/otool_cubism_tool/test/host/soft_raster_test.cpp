#include "soft_raster.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>

using namespace otool::cubism::renderer;

namespace {

uint16_t opaque4444(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)((r << 12) | (g << 8) | (b << 4) | 0x0F);
}

void test_barycentric_uv_vertex_mapping()
{
    uint16_t texture[16];
    for (uint32_t y = 0; y < 4; ++y) {
        for (uint32_t x = 0; x < 4; ++x) {
            texture[y * 4 + x] = opaque4444((uint8_t)(x + 1),
                                             (uint8_t)(y + 1), 0);
        }
    }

    uint16_t pixel = 0;
    frame_buffer fb = {&pixel, 1, 1};
    texture_ref tex = {texture, 4, 4};
    const float p0[2] = {0.0f, 0.0f};
    const float p1[2] = {3.0f, 0.0f};
    const float p2[2] = {0.0f, 3.0f};
    const float uv0[2] = {0.0f, 0.0f};
    const float uv1[2] = {1.0f, 0.0f};
    const float uv2[2] = {0.0f, 1.0f};

    draw_triangle(fb, tex, p0, p1, p2, uv0, uv1, uv2, 1.0f);
    /* Pixel center (0.5,0.5) maps to UV (1/6,1/6), nearest texel (1,1). */
    assert(pixel == 0x2100U);

    pixel = 0;
    draw_triangle(fb, tex, p0, p2, p1, uv0, uv2, uv1, 1.0f);
    assert(pixel == 0x2100U);
}

void test_rgba4444_pma_to_rgb565()
{
    uint16_t pixel = 0;
    frame_buffer fb = {&pixel, 1, 1};
    const float p0[2] = {0.0f, 0.0f};
    const float p1[2] = {2.0f, 0.0f};
    const float p2[2] = {0.0f, 2.0f};
    const float uv[2] = {0.0f, 0.0f};

    const uint16_t red = 0xF00FU;
    texture_ref tex = {&red, 1, 1};
    draw_triangle(fb, tex, p0, p1, p2, uv, uv, uv, 1.0f);
    assert(pixel == 0xF800U);

    pixel = 0;
    draw_triangle(fb, tex, p0, p1, p2, uv, uv, uv, 0.5f);
    const uint32_t r5 = (pixel >> 11) & 0x1FU;
    assert(r5 >= 16U && r5 <= 17U);
    assert((pixel & 0x07FFU) == 0);
}

void test_blend_modes_and_clip_mask()
{
    uint16_t pixel = 0x001FU;
    frame_buffer fb = {&pixel, 1, 1};
    const float p0[2] = {0.0f, 0.0f};
    const float p1[2] = {2.0f, 0.0f};
    const float p2[2] = {0.0f, 2.0f};
    const float uv[2] = {0.0f, 0.0f};
    const uint16_t red = 0xF00FU;
    texture_ref tex = {&red, 1, 1};
    const uint16_t indices[3] = {0, 1, 2};
    const float positions[6] = {0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 2.0f};
    const float uvs[6] = {};

    draw_triangle(fb, tex, p0, p1, p2, uv, uv, uv, 1.0f,
                  mesh_blend_mode::additive);
    assert(pixel == 0xF81FU);

    pixel = 0xFFFFU;
    draw_triangle(fb, tex, p0, p1, p2, uv, uv, uv, 1.0f,
                  mesh_blend_mode::multiplicative);
    assert(pixel == 0xF800U);

    uint8_t clip = 0;
    alpha_buffer mask = {&clip, 1, 1};
    draw_mask_mesh(fb, mask, tex, positions, uvs, indices, 3, 3, 1.0f);
    assert(clip == 0xFFU);

    clip = 0;
    const uint16_t white = 0xFFFFU;
    texture_ref white_tex = {&white, 1, 1};
    pixel = 0;
    fb.clip_mask = &clip;
    draw_triangle(fb, white_tex, p0, p1, p2, uv, uv, uv, 1.0f);
    assert(pixel == 0);

    fb.clip_mask_inverted = true;
    draw_triangle(fb, white_tex, p0, p1, p2, uv, uv, uv, 1.0f);
    assert(pixel == 0xFFFFU);
}

} // namespace

int main()
{
    test_barycentric_uv_vertex_mapping();
    test_rgba4444_pma_to_rgb565();
    test_blend_modes_and_clip_mask();
    std::puts("soft_raster_test: OK");
    return 0;
}
