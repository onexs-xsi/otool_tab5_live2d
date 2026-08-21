// otool_tab5_live2d console: interactive command line over USB-Serial-JTAG.
// Commands: help, free, version, wifi status/reconnect, llm ask/cancel/status.

#include "llm_app.h"
#include "wifi_app.h"

#include "esp_console.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linenoise/linenoise.h"
#include "sdkconfig.h"

#include <cstdio>
#include <cstring>

static const char *TAG = "console";

/* ---------------- wifi ---------------- */

static int do_wifi_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta == nullptr) {
        printf("wifi: sta netif not found\n");
        return 1;
    }
    esp_netif_ip_info_t ip = {};
    if (esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0) {
        printf("wifi: connected ip=" IPSTR "\n", IP2STR(&ip.ip));
    } else {
        printf("wifi: not connected\n");
    }
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        printf("wifi: ssid=%s rssi=%d channel=%d\n", (const char *)ap.ssid, ap.rssi, ap.primary);
    }
    return 0;
}

static int do_wifi_reconnect(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_err_t err = wifi_app_reconnect();
    printf("wifi: reconnect -> %s\n", esp_err_to_name(err));
    return 0;
}

/* ---------------- llm ---------------- */

static int do_llm_ask(int argc, char **argv)
{
    if (argc > 1) {
        /* llm-ask <text>：直接使用自定义问题 */
        llm_app_ask_text(argv[1]);
        printf("llm: asking: %s\n", argv[1]);
        return 0;
    }

    /* llm-ask（无参数）：二次确认后才使用默认问题；回车或 y 确认，其他/Ctrl+C 取消 */
    char *line = linenoise("Use default question? [y/N] (Enter/y to confirm, Ctrl+C cancels): ");
    if (line == nullptr) {
        printf("llm: cancelled (Ctrl+C)\n");
        return 0;
    }
    if (line[0] == '\0' || line[0] == 'y' || line[0] == 'Y') {
        llm_app_ask_now();
        printf("llm: asking default question\n");
    } else {
        printf("llm: cancelled\n");
    }
    return 0;
}

static int do_llm_cancel(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    llm_app_cancel_now();
    printf("llm: cancel requested\n");
    return 0;
}

static int do_llm_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    llm_app_status_t st;
    llm_app_get_status(&st);
    printf("llm: round=%d busy=%d reply_len=%u err=%s\n", st.round, (int)st.busy,
           (unsigned)st.reply_len, st.error[0] ? st.error : "-");
    return 0;
}

/* ---------------- system helpers ---------------- */

static int do_free(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("free heap: %u bytes, largest block: %u bytes\n",
           (unsigned)esp_get_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return 0;
}

static int do_version(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("otool_tab5_live2d %s\n", IDF_VER);
    return 0;
}

/* ---------------- init ---------------- */

static void register_commands(void)
{
    esp_console_register_help_command();

    esp_console_cmd_t cmd = {};

    cmd.command = "wifi";
    cmd.help = "wifi status";
    cmd.func = &do_wifi_status;
    esp_console_cmd_register(&cmd);

    cmd.command = "wifi-reconnect";
    cmd.help = "disconnect and reconnect STA";
    cmd.func = &do_wifi_reconnect;
    esp_console_cmd_register(&cmd);

    cmd.command = "llm-ask";
    cmd.help = "ask a question: 'llm-ask <text>'; without text, confirm default (Ctrl+C cancels)";
    cmd.func = &do_llm_ask;
    esp_console_cmd_register(&cmd);

    cmd.command = "llm-cancel";
    cmd.help = "cancel in-flight LLM request";
    cmd.func = &do_llm_cancel;
    esp_console_cmd_register(&cmd);

    cmd.command = "llm-status";
    cmd.help = "LLM worker status";
    cmd.func = &do_llm_status;
    esp_console_cmd_register(&cmd);

    cmd.command = "free";
    cmd.help = "show free heap";
    cmd.func = &do_free;
    esp_console_cmd_register(&cmd);

    cmd.command = "version";
    cmd.help = "show firmware version";
    cmd.func = &do_version;
    esp_console_cmd_register(&cmd);
}

extern "C" void console_start(void)
{
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.max_cmdline_length = 256;
    repl_cfg.prompt = "tab5> ";

    esp_console_dev_usb_serial_jtag_config_t dev_cfg = {};

    esp_console_repl_t *repl = nullptr;
    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "new repl usb_serial_jtag: %s", esp_err_to_name(err));
        return;
    }

    register_commands();

    err = esp_console_start_repl(repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start repl: %s", esp_err_to_name(err));
    }
}
