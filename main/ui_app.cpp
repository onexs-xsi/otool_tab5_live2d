// otool_tab5_live2d UI module: LVGL screen for agent status and reply.
// 不含 LLM/网络逻辑；通过 agent_app_status()/agent_app_reply_read() 读取数据，
// 触摸点击取消当前 run 并触发一次 agent 问询（agent_app_cancel + agent_app_ask）。

#include "ui_app.h"
#include "agent_app.h"

#include "otool_lvgl_port.h"
#include "lvgl.h"
#include "src/font/binfont_loader/lv_binfont_loader.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#include <cstdio>

static const char *TAG = "ui_app";

/* 默认问询：直接走 agent API（不经 console/linenoise），中文安全 */
#define UI_DEFAULT_QUESTION \
    "请调用 get_device_status 工具查看设备状态，然后用一句话中文总结。"

/* CMake EMBED_FILES 嵌入的二进制字体（main/fonts/noto_cn_fonts/）。
 * 注意：IDF 的 embed 符号名只取文件名（目录被剥离）。
 * 仅嵌入回复区使用的 24px 中文；状态/提示行为英文内容，使用默认 Montserrat，
 * 以最小化固件体积。 */
extern const uint8_t _binary_noto_zh_mid_24_bin_start[];
extern const uint8_t _binary_noto_zh_mid_24_bin_end[];

static lv_font_t *s_font_24 = nullptr;   /* 回复区 */

static lv_obj_t *s_status_label = nullptr;
static lv_obj_t *s_reply_label = nullptr;
static lv_obj_t *s_hint_label = nullptr;

static char s_status_text[192] = "starting...";
static char s_reply_text[4096 + 8] = "";

static void fonts_load(void)
{
    s_font_24 = lv_binfont_create_from_buffer((void *)_binary_noto_zh_mid_24_bin_start,
                                              (uint32_t)(_binary_noto_zh_mid_24_bin_end -
                                                         _binary_noto_zh_mid_24_bin_start));
    if (s_font_24 == nullptr) {
        ESP_LOGE(TAG, "noto_zh_mid_24.bin load failed");
    }
}

/* 点击屏幕：打断当前 agent run 并触发一次默认问询 */
static void screen_click_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "tap detected");
    agent_app_cancel(); /* 立即中断进行中的 run（若在跑） */
    agent_app_ask(UI_DEFAULT_QUESTION);
}

static void ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_status_label == nullptr) {
        return;
    }

    char status[192] = { 0 };
    agent_app_status(status, sizeof(status));
    agent_app_reply_read(s_reply_text, sizeof(s_reply_text));

    if (s_reply_text[0] == '\0') {
        snprintf(s_reply_text, sizeof(s_reply_text), "（暂无回复，点击屏幕提问）");
    }

    const char *proto = agent_proto_name();
    snprintf(s_status_text, sizeof(s_status_text), "agent [%s] | %s", proto, status);
    lv_label_set_text(s_status_label, s_status_text);
    lv_label_set_text(s_reply_label, s_reply_text);
}

extern "C" void ui_app_start(void)
{
    fonts_load();

    otool_lvgl_port_lock(0);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_pad_all(scr, 16, 0);

    /* 状态行（英文内容，默认字体） */
    s_status_label = lv_label_create(scr);
    lv_label_set_text(s_status_label, "starting...");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x8fa3c0), 0);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_LEFT, 0, 0);

    /* 触摸提示行（英文内容，默认字体） */
    s_hint_label = lv_label_create(scr);
    lv_label_set_text(s_hint_label, "tap screen to ask / interrupt");
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(0x55607a), 0);
    lv_obj_align(s_hint_label, LV_ALIGN_TOP_RIGHT, 0, 0);

    /* 回复区（中文，Noto 24px） */
    s_reply_label = lv_label_create(scr);
    lv_label_set_text(s_reply_label, "");
    lv_label_set_long_mode(s_reply_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_reply_label, lv_pct(100));
    lv_obj_set_style_text_color(s_reply_label, lv_color_white(), 0);
    if (s_font_24 != nullptr) {
        lv_obj_set_style_text_font(s_reply_label, s_font_24, 0);
    }
    lv_obj_align(s_reply_label, LV_ALIGN_TOP_LEFT, 0, 40);
    lv_obj_set_height(s_reply_label, lv_pct(80));
    lv_obj_set_style_text_line_space(s_reply_label, 6, 0);

    /* 全屏触摸：点击提问 / 打断 */
    lv_obj_add_event_cb(scr, screen_click_cb, LV_EVENT_CLICKED, nullptr);

    otool_lvgl_port_unlock();

    lv_timer_create(ui_timer_cb, CONFIG_OTOOL_LLM_UI_REFRESH_MS, nullptr);
}
