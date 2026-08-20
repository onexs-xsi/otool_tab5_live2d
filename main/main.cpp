// otool_tab5_live2d 最小演示：把 Mao 模型（自研 self Core 管线）渲染到 Tab5 屏幕。
// 初始化流程与 tab5_breath_control_tool 相同：NVS → M5Autodetect → 硬件 → LVGL。

#include "M5Autodetect.h"
#include "otool_tab5_component.h"
#include "otool_lvgl_port.h"
#include "lvgl.h"

#include "otool_cubism_demo.h"
#include "otool_cubism_tool.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sdkconfig.h"

static const char *TAG = "main";

static m5::tab5::otool_tab5_component g_comp;

/* ------------------------------------------------------------------ */
/* 模型演示：嵌入式素材（构建期从 %TEMP% 嵌入，见 main/CMakeLists.txt）   */
/* ------------------------------------------------------------------ */

extern const uint8_t mao_moc3_start[] asm("_binary_Mao_moc3_start");
extern const uint8_t mao_moc3_end[] asm("_binary_Mao_moc3_end");
extern const uint8_t mao_tex_start[] asm("_binary_mao_tex_raw_start");
extern const uint8_t mao_tex_end[] asm("_binary_mao_tex_raw_end");

namespace {
constexpr uint16_t FB_W = 1280, FB_H = 720; /* 与 LVGL 逻辑分辨率一致 */
constexpr int32_t  P_ANGLE_X = 0;           /* Mao 参数索引（cdi3 顺序） */
constexpr int32_t  P_ANGLE_Y = 1;
constexpr int32_t  P_EYE_L_OPEN = 5;
constexpr int32_t  P_EYE_R_OPEN = 8;

uint16_t *s_fb = nullptr;                   /* PSRAM 帧缓冲（RGB565） */
lv_obj_t *s_img = nullptr;
lv_image_dsc_t s_img_dsc = {};
otool::cubism::demo::model_handle *s_model = nullptr;
} // namespace

static void demo_model_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    float t = 0.0f;
    const float period = 3.4f;              /* 眨眼周期（s） */
    while (true) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(50));
        t += 0.05f;

        /* 轻微头部摆动 + 周期性眨眼 */
        const float ang_x = 8.0f * sinf(t * 0.6f);
        const float ang_y = 6.0f * sinf(t * 0.8f + 1.2f);
        float blink = 1.0f;
        const float ph = fmodf(t, period);
        if (ph < 0.18f) {
            blink = 1.0f - ph / 0.18f;      /* 闭眼 */
        } else if (ph < 0.36f) {
            blink = (ph - 0.18f) / 0.18f;   /* 睁眼 */
        }
        otool::cubism::demo::model_set_param(s_model, P_ANGLE_X, ang_x);
        otool::cubism::demo::model_set_param(s_model, P_ANGLE_Y, ang_y);
        otool::cubism::demo::model_set_param(s_model, P_EYE_L_OPEN, blink);
        otool::cubism::demo::model_set_param(s_model, P_EYE_R_OPEN, blink);

        if (!otool::cubism::demo::model_render(s_model, s_fb, FB_W, FB_H)) {
            ESP_LOGE(TAG, "model_render failed");
            break;
        }

        otool_lvgl_port_lock(0);
        lv_obj_invalidate(s_img);
        otool_lvgl_port_unlock();
    }
    vTaskDelete(nullptr);
}

static void demo_cubism_model(void)
{
    /* 帧缓冲：PSRAM */
    s_fb = (uint16_t *)heap_caps_malloc((size_t)FB_W * FB_H * 2,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_fb == nullptr) {
        ESP_LOGE(TAG, "no PSRAM for framebuffer");
        return;
    }

    /* 素材：构建期嵌入的 moc3 + RGBA4444 纹理 */
    otool::cubism::demo::model_asset asset = {};
    asset.moc3 = mao_moc3_start;
    asset.moc3_size = (size_t)(mao_moc3_end - mao_moc3_start);
    asset.tex = mao_tex_start;
    asset.tex_w = 1024;
    asset.tex_h = 1024;

    s_model = otool::cubism::demo::model_create(asset);
    if (s_model == nullptr) {
        ESP_LOGE(TAG, "model_create failed");
        return;
    }

    /* LVGL image 直接引用帧缓冲（无拷贝） */
    s_img_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_img_dsc.header.w = FB_W;
    s_img_dsc.header.h = FB_H;
    s_img_dsc.header.stride = FB_W * 2;
    s_img_dsc.data_size = (uint32_t)FB_W * FB_H * 2;
    s_img_dsc.data = reinterpret_cast<const uint8_t *>(s_fb);

    otool_lvgl_port_lock(0);
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    s_img = lv_image_create(scr);
    lv_image_set_src(s_img, &s_img_dsc);
    lv_obj_set_pos(s_img, 0, 0);
    otool_lvgl_port_unlock();

    ESP_LOGI(TAG, "model demo ready: params=%d", otool::cubism::demo::model_param_count(s_model));

    BaseType_t ok = xTaskCreate(demo_model_task, "cubism_demo", 8192, nullptr, 5, nullptr);
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_FAIL);
}

extern "C" void app_main(void)
{
    // NVS 初始化
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // 自动检测设备型号
    M5Autodetect autodetect;
    autodetect.begin(M5Autodetect::debug_info);
    const m5::autodetect::DeviceInfo *info = autodetect.detect();
    if (!info) {
        ESP_LOGE(TAG, "No device detected");
        return;
    }
    ESP_LOGI(TAG, "Detected: %s (board_id=%d)", info->name, info->board_id);

    // 根据 board_id 选择 variant
    m5::tab5::M5TAB5_VariantId variant_id;
    switch (info->board_id) {
    case m5::autodetect::board_M5Tab5_ST7123:
        variant_id = m5::tab5::M5TAB5_VARIANT_TAB5_LCD_ST7123_TOUCH_ST7123;
        break;
    case m5::autodetect::board_M5Tab5_IlI9881c:
        variant_id = m5::tab5::M5TAB5_VARIANT_TAB5_LCD_ILI9881_TOUCH_GT911;
        break;
    case m5::autodetect::board_M5Tab5_ST7121:
        variant_id = m5::tab5::M5TAB5_VARIANT_TAB5_LCD_ST7121_TOUCH_ST7121;
        break;
    default:
        ESP_LOGW(TAG, "Unknown board_id=%d, using reference variant", info->board_id);
        variant_id = m5::tab5::M5TAB5_VARIANT_REFERENCE;
        break;
    }

    // 初始化硬件（显示 + 触摸 + IO 扩展器）
    m5::tab5::otool_tab5_component_config_t cfg = {};
    cfg.variant_id              = variant_id;
    cfg.enable_optional_drivers = true;

    err = g_comp.begin(cfg);
    ESP_ERROR_CHECK(err);
    ESP_LOGI(TAG, "Hardware initialized: %s", g_comp.variant()->id);

    // 初始化 LVGL 端口 + 显示 + 触摸（逻辑分辨率 1280x720）
    err = g_comp.lvgl_init();
    ESP_ERROR_CHECK(err);

    // 模型演示：自研 Core 渲染 → LVGL image 上屏
    demo_cubism_model();
}
