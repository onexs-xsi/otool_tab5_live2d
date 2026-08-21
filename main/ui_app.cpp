// otool_tab5_live2d UI module: LVGL screen for LLM status and reply.
// 不含 LLM/网络逻辑；通过 llm_app_get_status()/llm_app_reply_read()/
// llm_app_hint_read() 读取数据，触摸点击通过 llm_app_ask_now() 触发。

#include "ui_app.h"
#include "llm_app.h"

#include "otool_lvgl_port.h"
#include "lvgl.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#include <cstdio>

static const char *TAG = "ui_app";

static lv_obj_t *s_status_label = nullptr;
static lv_obj_t *s_reply_label = nullptr;
static lv_obj_t *s_hint_label = nullptr;

static char s_status_text[192] = "starting...";
static char s_reply_text[4096 + 8] = "";
static char s_hint_text[128] = "";

/* 点击屏幕：触发提问 / 打断当前请求 */
static void screen_click_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "tap detected");
    llm_app_ask_now();
}

static void ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_status_label == nullptr) {
        return;
    }

    llm_app_status_t st;
    llm_app_get_status(&st);
    llm_app_reply_read(s_reply_text, sizeof(s_reply_text));
    llm_app_hint_read(s_hint_text, sizeof(s_hint_text));

    if (st.error[0] != '\0') {
        snprintf(s_status_text, sizeof(s_status_text), "round %d | error: %.120s", st.round,
                 st.error);
    } else if (st.busy) {
        snprintf(s_status_text, sizeof(s_status_text), "round %d | LLM streaming...", st.round);
    } else {
        snprintf(s_status_text, sizeof(s_status_text), "round %d | LLM idle | tap to ask",
                 st.round);
    }

    lv_label_set_text(s_status_label, s_status_text);
    lv_label_set_text(s_reply_label, s_reply_text);
    if (s_hint_text[0] != '\0') {
        lv_label_set_text(s_hint_label, s_hint_text);
    }
}

extern "C" void ui_app_start(void)
{
    otool_lvgl_port_lock(0);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_pad_all(scr, 16, 0);

    /* 状态行 */
    s_status_label = lv_label_create(scr);
    lv_label_set_text(s_status_label, "starting...");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x8fa3c0), 0);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_LEFT, 0, 0);

    /* 触摸提示行 */
    s_hint_label = lv_label_create(scr);
    lv_label_set_text(s_hint_label, "tap screen to ask / interrupt");
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(0x55607a), 0);
    lv_obj_align(s_hint_label, LV_ALIGN_TOP_RIGHT, 0, 0);

    /* 回复区 */
    s_reply_label = lv_label_create(scr);
    lv_label_set_text(s_reply_label, "");
    lv_label_set_long_mode(s_reply_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_reply_label, lv_pct(100));
    lv_obj_set_style_text_color(s_reply_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_reply_label, &lv_font_source_han_sans_sc_16_cjk, 0);
    lv_obj_align(s_reply_label, LV_ALIGN_TOP_LEFT, 0, 40);
    lv_obj_set_height(s_reply_label, lv_pct(80));
    lv_obj_set_style_text_line_space(s_reply_label, 6, 0);

    /* 全屏触摸：点击提问 / 打断 */
    lv_obj_add_event_cb(scr, screen_click_cb, LV_EVENT_CLICKED, nullptr);

    otool_lvgl_port_unlock();

    lv_timer_create(ui_timer_cb, CONFIG_OTOOL_LLM_UI_REFRESH_MS, nullptr);
}
