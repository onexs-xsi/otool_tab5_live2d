// otool_tab5_live2d console: interactive command line over USB-Serial-JTAG.
// Commands: help, free, version, wifi status/reconnect, llm ask/cancel/status.

#include "llm_app.h"
#include "wifi_app.h"
#include "credential_store.h"
#include "agent_app.h"

#include "esp_console.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linenoise/linenoise.h"
#include "nvs.h"
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
        /* llm-ask <text...>：拼接全部参数作为完整问题（支持含空格的中文/英文） */
        char question[256];
        size_t pos = 0;
        for (int i = 1; i < argc && pos < sizeof(question) - 1; i++) {
            if (i > 1 && pos < sizeof(question) - 2) {
                question[pos++] = ' ';
            }
            size_t n = strlen(argv[i]);
            if (n > sizeof(question) - 1 - pos) {
                n = sizeof(question) - 1 - pos;
            }
            memcpy(question + pos, argv[i], n);
            pos += n;
        }
        question[pos] = '\0';
        llm_app_ask_text(question);
        printf("llm: run queued (%u bytes)\n", (unsigned)strlen(question));
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

static int do_stack(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    static const char *names[] = { "agent_worker", "llm_worker", "lvgl_task", "repl" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        TaskHandle_t t = xTaskGetHandle(names[i]);
        if (t != nullptr) {
            printf("stack %s: hwm=%lu bytes\n", names[i],
                   (unsigned long)uxTaskGetStackHighWaterMark(t));
        }
    }
    return 0;
}

/* ---------------- cred（有效凭证；sdkconfig 优先，NVS 后备） ---------------- */

static const char *cred_valid_names[] = { "wifi_ssid", "wifi_pass", "llm_key" };

static bool cred_name_valid(const char *name)
{
    for (size_t i = 0; i < sizeof(cred_valid_names) / sizeof(cred_valid_names[0]); i++) {
        if (strcmp(name, cred_valid_names[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void cred_print_masked(const char *name, const char *value)
{
    size_t len = strlen(value);
    if (len == 0) {
        printf("  %s = <empty>\n", name);
        return;
    }
    /* SSID/密码/Key 均不显示任何前缀，短值也不能原样泄漏。 */
    printf("  %s = <set> (%u chars)\n", name, (unsigned)len);
}

static int do_cred_set(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: cred-set <name> <value>   (names: wifi_ssid|wifi_pass|llm_key)\n");
        return 1;
    }
    if (!cred_name_valid(argv[1])) {
        printf("cred: unknown name '%s' (use wifi_ssid|wifi_pass|llm_key)\n", argv[1]);
        return 1;
    }
    esp_err_t err = credential_store_set(argv[1], argv[2]);
    if (err != ESP_OK) {
        printf("cred: set failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("cred: %s saved to NVS fallback; a non-empty sdkconfig value still takes precedence\n",
           argv[1]);
    return 0;
}

static int do_cred_show(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    for (size_t i = 0; i < sizeof(cred_valid_names) / sizeof(cred_valid_names[0]); i++) {
        char buf[256];
        if (credential_store_copy_runtime(cred_valid_names[i], buf, sizeof(buf)) == ESP_OK) {
            cred_print_masked(cred_valid_names[i], buf);
        } else {
            printf("  %s = <unset>\n", cred_valid_names[i]);
        }
        volatile char *p = buf;
        for (size_t j = 0; j < sizeof(buf); j++) {
            p[j] = '\0';
        }
    }
    return 0;
}

static int do_cred_clear(int argc, char **argv)
{
    if (argc < 2 || !cred_name_valid(argv[1])) {
        printf("usage: cred-clear <name>   (names: wifi_ssid|wifi_pass|llm_key)\n");
        return 1;
    }
    esp_err_t err = credential_store_erase(argv[1]);
    if (err != ESP_OK) {
        printf("cred: clear failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("cred: %s erased from NVS fallback; sdkconfig value is unchanged\n", argv[1]);
    return 0;
}

/* ---------------- agent ---------------- */

static int do_agent_run(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: agent <text...>   (tool-enabled agent run; Ctrl+C via agent-cancel)\n");
        return 1;
    }
    char question[256];
    size_t pos = 0;
    for (int i = 1; i < argc && pos < sizeof(question) - 1; i++) {
        if (i > 1 && pos < sizeof(question) - 2) {
            question[pos++] = ' ';
        }
        size_t n = strlen(argv[i]);
        if (n > sizeof(question) - 1 - pos) {
            n = sizeof(question) - 1 - pos;
        }
        memcpy(question + pos, argv[i], n);
        pos += n;
    }
    question[pos] = '\0';
    agent_app_ask(question);
    printf("agent: run queued (%u bytes)\n", (unsigned)strlen(question));
    return 0;
}

static int do_agent_cancel(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    agent_app_cancel();
    printf("agent: cancel requested\n");
    return 0;
}

static int do_agent_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    char buf[192];
    agent_app_status(buf, sizeof(buf));
    printf("agent: %s\n", buf);
    return 0;
}

static int do_agent_reset(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    agent_app_reset_session();
    printf("agent: conversation reset queued\n");
    return 0;
}

static int do_agent_protocol(int argc, char **argv)
{
    if (argc < 2) {
        printf("agent-protocol: %s (restart to apply)\n", agent_proto_name());
        printf("usage: agent-protocol <responses|chat>\n");
        return 0;
    }
    if (strcmp(argv[1], "responses") != 0 && strcmp(argv[1], "chat") != 0) {
        printf("agent-protocol: invalid '%s' (use responses|chat)\n", argv[1]);
        return 1;
    }
    nvs_handle_t h;
    if (nvs_open("otool_cfg", NVS_READWRITE, &h) != ESP_OK) {
        printf("agent-protocol: nvs open failed\n");
        return 1;
    }
    esp_err_t err = nvs_set_str(h, "agent_proto", argv[1]);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        printf("agent-protocol: save failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("agent-protocol: set to %s (restart to apply)\n", argv[1]);
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

    cmd.command = "stack";
    cmd.help = "task stack high-water marks";
    cmd.func = &do_stack;
    esp_console_cmd_register(&cmd);

    cmd.command = "version";
    cmd.help = "show firmware version";
    cmd.func = &do_version;
    esp_console_cmd_register(&cmd);

    cmd.command = "cred";
    cmd.help = "show effective credentials, sdkconfig first (masked)";
    cmd.func = &do_cred_show;
    esp_console_cmd_register(&cmd);

    cmd.command = "cred-set";
    cmd.help = "store optional NVS fallback: cred-set <wifi_ssid|wifi_pass|llm_key> <value>";
    cmd.func = &do_cred_set;
    esp_console_cmd_register(&cmd);

    cmd.command = "cred-clear";
    cmd.help = "erase an NVS fallback: cred-clear <wifi_ssid|wifi_pass|llm_key>";
    cmd.func = &do_cred_clear;
    esp_console_cmd_register(&cmd);

    cmd.command = "agent";
    cmd.help = "tool-enabled agent run: agent <text...>";
    cmd.func = &do_agent_run;
    esp_console_cmd_register(&cmd);

    cmd.command = "agent-cancel";
    cmd.help = "cancel the active agent run";
    cmd.func = &do_agent_cancel;
    esp_console_cmd_register(&cmd);

    cmd.command = "agent-status";
    cmd.help = "agent worker status";
    cmd.func = &do_agent_status;
    esp_console_cmd_register(&cmd);

    cmd.command = "agent-reset";
    cmd.help = "cancel the active run and clear agent conversation state";
    cmd.func = &do_agent_reset;
    esp_console_cmd_register(&cmd);

    cmd.command = "agent-protocol";
    cmd.help = "agent protocol: agent-protocol <responses|chat> (restart to apply)";
    cmd.func = &do_agent_protocol;
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
