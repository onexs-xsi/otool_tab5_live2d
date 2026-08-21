// otool_tab5_live2d 最小演示：白屏 + hello onexs。
// 初始化流程与 tab5_breath_control_tool 相同：NVS → M5Autodetect → 硬件 → LVGL。

#include "M5Autodetect.h"
#include "otool_tab5_component.h"
#include "otool_lvgl_port.h"
#include "lvgl.h"

#include "esp_log.h"
#include "nvs_flash.h"

#include "sdkconfig.h"

static const char *TAG = "main";

static m5::tab5::otool_tab5_component g_comp;

/* ------------------------------------------------------------------ */
/* 白屏 + hello onexs 界面                                              */
/* ------------------------------------------------------------------ */

static void show_hello_onexs(void)
{
    otool_lvgl_port_lock(0);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);

    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, "hello onexs");
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_center(label);

    otool_lvgl_port_unlock();
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

    // 白屏 + hello onexs
    show_hello_onexs();
}
