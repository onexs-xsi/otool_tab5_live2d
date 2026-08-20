#pragma once

#include "esp_system.h"

/* lv_conf_internal.h 可能已将 LV_ASSERT_HANDLER 定义为 while(1)，
 * 此处覆盖为 esp_restart()，防止 lvgl_task 断言失败后死循环占满 CPU */
#undef LV_ASSERT_HANDLER
#define LV_ASSERT_HANDLER esp_restart();
