// otool_tab5_live2d 最小演示：白色屏幕 + 居中显示 "hello onexs."
// 初始化流程与 tab5_breath_control_tool 相同：NVS → M5Autodetect → 硬件 → LVGL。

#include "M5Autodetect.h"
#include "otool_tab5_component.h"
#include "otool_lvgl_port.h"
#include "lvgl.h"

#include "otool_cubism_tool.h"
#include "otool_cubism_port.h"

#include "esp_log.h"
#include "nvs_flash.h"

#include "sdkconfig.h"

static const char *TAG = "main";

static m5::tab5::otool_tab5_component g_comp;

/* ------------------------------------------------------------------ */
/* otool_cubism_tool 最小生命周期演示（S0）                              */
/*                                                                     */
/* S1 尚未提供真实 display lease，因此这里使用占位端口：acquire 明确失败，  */
/* 组件必须拒绝接管显示（可行性报告 §12-3）。                              */
/* ------------------------------------------------------------------ */

static esp_err_t stub_display_acquire(void *ctx)
{
    (void)ctx;
    return ESP_ERR_NOT_SUPPORTED; /* S1 实现真实租约前，显示不可接管 */
}

static esp_err_t stub_display_release(void *ctx)
{
    (void)ctx;
    return ESP_OK;
}

static esp_err_t stub_display_get_info(void *ctx, otool_cubism_display_info_t *out)
{
    (void)ctx;
    if (out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    out->phys_width      = 720;
    out->phys_height     = 1280;
    out->logical_width   = 1280;
    out->logical_height  = 720;
    out->bytes_per_pixel = 2;
    return ESP_OK;
}

static const otool_cubism_display_port_t s_stub_display_port = {
    .ctx = nullptr,
    .acquire = stub_display_acquire,
    .release = stub_display_release,
    .present = nullptr,
    .get_info = stub_display_get_info,
};

static void demo_cubism_lifecycle(void)
{
    static otool::cubism::otool_cubism_tool tool;

    otool_cubism_config_t cfg = {};
    cfg.display = &s_stub_display_port;
    cfg.storage = nullptr; /* S2 接入素材端口 */

    esp_err_t err = tool.init(cfg);
    ESP_LOGI(TAG, "cubism init -> %s", esp_err_to_name(err));
    if (err != ESP_OK) {
        return;
    }

    otool_cubism_status_t st = {};
    tool.get_status(&st);
    ESP_LOGI(TAG, "cubism state=%d package_version=%lu",
             (int)st.state, (unsigned long)st.package_version);

    /* 无 storage 端口 → 明确失败，不静默降级 */
    err = tool.load_package("/sdcard/live2d/manifest.bin");
    ESP_LOGI(TAG, "cubism load_package (no storage) -> %s", esp_err_to_name(err));

    /* start 需要 LOADED + 显示租约；租约不可用 → 拒绝接管显示 */
    err = tool.start(OTOOL_CUBISM_MODE_CLIP_PLAYER);
    ESP_LOGI(TAG, "cubism start (lease unavailable) -> %s", esp_err_to_name(err));

    err = tool.deinit();
    ESP_LOGI(TAG, "cubism deinit -> %s", esp_err_to_name(err));
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

    // 创建 UI：白色背景 + 居中文本（须在 LVGL 锁内执行）
    otool_lvgl_port_lock(0);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);

    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, "hello onexs.");
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_36, 0);
    lv_obj_center(label);

    otool_lvgl_port_unlock();

    ESP_LOGI(TAG, "UI ready");

    demo_cubism_lifecycle();
}
