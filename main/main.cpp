// otool_tab5_live2d：Tab5 最小演示 → 联网（C6 Wi-Fi）+ 豆包 LLM 流式对话。
// 初始化流程：NVS → M5Autodetect → 硬件 → LVGL → 可选网络/LLM → UI/console。

#include "M5Autodetect.h"
#include "otool_tab5_component.h"
#include "otool_lvgl_port.h"
#include "lvgl.h"

#include "esp_log.h"
#include "nvs_flash.h"

#include "sdkconfig.h"

#include "llm_app.h"
#include "wifi_app.h"
#include "ui_app.h"
#include "credential_store.h"
#include "agent_app.h"
#include "console_cmds.h"

static const char *TAG = "main";

static m5::tab5::otool_tab5_component g_comp;

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

    // 有效凭证：本地 sdkconfig 优先，NVS 仅作配置为空时的可选后备。
    ESP_ERROR_CHECK(credential_store_init());

    // 联网失败（包括未配置 SSID）进入离线模式，不能阻止 UI/console 启动。
    err = wifi_app_start(&g_comp);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi unavailable (%s); continuing in offline mode",
                 esp_err_to_name(err));
    }

    // LLM 对话 worker（独立模块 llm_app）
    llm_app_start();

    // LLM 状态界面（独立模块 ui_app）
    ui_app_start(&g_comp);

    // Agent（工具调用）worker（独立模块 agent_app）
    agent_app_start();

    // USB-Serial-JTAG 命令行（help / wifi / llm / agent / cred / free / version）
    console_start();
}
