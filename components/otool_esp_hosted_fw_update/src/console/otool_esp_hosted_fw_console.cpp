#include "sdkconfig.h"

#ifdef CONFIG_OTOOL_ESP_HOSTED_FW_ENABLE_CONSOLE_CMDS

#include "otool_esp_hosted_fw_console.h"
#include "otool_esp_hosted_fw_update.h"

#include "esp_console.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_hosted.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "fw_console";

/* ------------------------------------------------------------------ */
/*  辅助：打印子命令帮助                                               */
/* ------------------------------------------------------------------ */

/* 前向声明（实现在下方） */
static int do_fw_list(void);
static int do_fw_info(void);
static int do_fw_flash(const char *name);

typedef struct {
    esp_err_t query_ret;
    esp_hosted_coprocessor_fwver_t fwver;
} fw_console_slave_info_t;

static const char *fw_original_name_at(OtoolEspHostedFwProvider *provider, size_t index)
{
    const char *original_name = provider->firmware_original_name_at(index);
    if (original_name != nullptr && original_name[0] != '\0') {
        return original_name;
    }
    return provider->firmware_name_at(index);
}

static fw_console_slave_info_t fw_console_query_slave_info(void)
{
    fw_console_slave_info_t info = {};
    info.query_ret = static_cast<esp_err_t>(esp_hosted_get_coprocessor_fwversion(&info.fwver));
    return info;
}

static bool fw_console_activate_supported(const esp_hosted_coprocessor_fwver_t &fwver)
{
    return (fwver.major1 > 2U) ||
           (fwver.major1 == 2U && fwver.minor1 > 5U);
}

static void fw_console_print_runtime_summary(size_t embedded_count)
{
    const uint32_t host_version = ESP_HOSTED_VERSION_VAL(
        ESP_HOSTED_VERSION_MAJOR_1,
        ESP_HOSTED_VERSION_MINOR_1,
        ESP_HOSTED_VERSION_PATCH_1);
    const fw_console_slave_info_t slave_info = fw_console_query_slave_info();

    printf("Runtime:\n");
    printf("  Host OTA component   %s\n", otool_esp_hosted_fw_update_version());
    printf("  Host ESP-Hosted      " ESP_HOSTED_VERSION_PRINTF_FMT "\n",
           ESP_HOSTED_VERSION_PRINTF_ARGS(host_version));
    printf("  Embedded images      %zu\n", embedded_count);

    if (slave_info.query_ret == ESP_OK) {
        printf("  Co-proc reachable    yes\n");
        printf("  Co-proc app version  %" PRIu32 ".%" PRIu32 ".%" PRIu32 "\n",
               slave_info.fwver.major1,
               slave_info.fwver.minor1,
               slave_info.fwver.patch1);
        printf("  OTA activate RPC     %s\n",
               fw_console_activate_supported(slave_info.fwver) ? "supported" : "legacy/unsupported");
    } else {
        printf("  Co-proc reachable    no (%s)\n", esp_err_to_name(slave_info.query_ret));
        printf("  Co-proc app version  unavailable\n");
        printf("  OTA activate RPC     unknown\n");
    }
}

static void print_fw_usage(void)
{
    printf("Usage:\n");
    printf("  fw list           - list all embedded firmware images\n");
    printf("  fw info           - query co-processor current firmware version\n");
    printf("  fw flash <name>   - OTA-flash the named image (reboots on success)\n");
}

/* ------------------------------------------------------------------ */
/*  快捷命令基础设施                                                   */
/* ------------------------------------------------------------------ */

#define MAX_FLASH_SHORTCUTS 8

/* 运行时填充：固件名称 / 命令名称 / help 字符串 */
static char s_sc_fw_name  [MAX_FLASH_SHORTCUTS][48];
static char s_sc_cmd_name [MAX_FLASH_SHORTCUTS][64];
static char s_sc_help     [MAX_FLASH_SHORTCUTS][96];
static size_t s_sc_count = 0;

/* C++ 不支持捕获变量的函数指针，故静态展开8个回调 */
static int sc_flash_0(int, char **) { return do_fw_flash(s_sc_fw_name[0]); }
static int sc_flash_1(int, char **) { return do_fw_flash(s_sc_fw_name[1]); }
static int sc_flash_2(int, char **) { return do_fw_flash(s_sc_fw_name[2]); }
static int sc_flash_3(int, char **) { return do_fw_flash(s_sc_fw_name[3]); }
static int sc_flash_4(int, char **) { return do_fw_flash(s_sc_fw_name[4]); }
static int sc_flash_5(int, char **) { return do_fw_flash(s_sc_fw_name[5]); }
static int sc_flash_6(int, char **) { return do_fw_flash(s_sc_fw_name[6]); }
static int sc_flash_7(int, char **) { return do_fw_flash(s_sc_fw_name[7]); }

typedef int (*console_func_t)(int, char **);
static const console_func_t s_sc_funcs[MAX_FLASH_SHORTCUTS] = {
    sc_flash_0, sc_flash_1, sc_flash_2, sc_flash_3,
    sc_flash_4, sc_flash_5, sc_flash_6, sc_flash_7,
};

/* fl / fi 快捷命令回调 */
static int cmd_fl(int, char **) { return do_fw_list(); }
static int cmd_fi(int, char **) { return do_fw_info(); }

/* ------------------------------------------------------------------ */
/*  fw list                                                            */
/* ------------------------------------------------------------------ */

static int do_fw_list(void)
{
    OtoolEspHostedFwProvider *p = OtoolEspHostedFwDefaultFactory::provider();
    size_t count = p->firmware_count();

    if (count == 0) {
        printf("No embedded firmware images available.\n");
        printf("Enable OTOOL_ESP_HOSTED_FW_PROVIDER_EMBEDDED or\n");
        printf("OTOOL_ESP_HOSTED_FW_EMBED_NETWORK_ADAPTER in menuconfig.\n");
        return 0;
    }

    printf("Embedded firmware images (%zu):\n", count);
    for (size_t i = 0; i < count; i++) {
        const char *name = p->firmware_name_at(i);
        const char *original_name = fw_original_name_at(p, i);
        otool_esp_hosted_fw_blob_t blob = {};
        size_t size = 0;
        if (p->get_blob(name, blob) == ESP_OK) {
            size = (size_t)(blob.end - blob.start);
        }
        printf("  [%zu]  %-32s  %7zu bytes  (%.1f KB)\n",
               i, original_name, size, (float)size / 1024.0f);
        if (strcmp(original_name, name) != 0) {
            printf("       id: %s\n", name);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  fw info                                                            */
/* ------------------------------------------------------------------ */

static int do_fw_info(void)
{
    esp_hosted_coprocessor_fwver_t fwver = {};
    esp_err_t ret = (esp_err_t)esp_hosted_get_coprocessor_fwversion(&fwver);
    if (ret != ESP_OK) {
        printf("Error: failed to query co-processor version: %s\n",
               esp_err_to_name(ret));
        return 1;
    }
    printf("Co-processor firmware version: %" PRIu32 ".%" PRIu32 ".%" PRIu32 "\n",
           fwver.major1, fwver.minor1, fwver.patch1);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  fw flash <name>                                                    */
/* ------------------------------------------------------------------ */

static int do_fw_flash(const char *name)
{
    if (name == nullptr || name[0] == '\0') {
        printf("Error: firmware name required.\n\n");
        do_fw_list();
        printf("\nUsage: fw flash <name>\n");
        return 1;
    }

    OtoolEspHostedFwProvider *p = OtoolEspHostedFwDefaultFactory::provider();

    /* 提前验证名称，给出友好提示后再开始传输 */
    otool_esp_hosted_fw_blob_t blob = {};
    if (p->get_blob(name, blob) != ESP_OK) {
        printf("Error: firmware '%s' not found.\n\n", name);
        do_fw_list();
        return 1;
    }

    size_t fw_size = (size_t)(blob.end - blob.start);
    printf("Starting OTA: '%s'  (%zu bytes / %.1f KB)\n",
           name, fw_size, (float)fw_size / 1024.0f);

    OtoolEspHostedFwUpdater updater(p, OtoolEspHostedFwDefaultFactory::transport());
    esp_err_t ret = updater.update_by_name(name);
    if (ret != ESP_OK) {
        printf("OTA failed: %s\n", esp_err_to_name(ret));
        return 1;
    }

    printf("OTA successful. Rebooting in 2 s...\n");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return 0; /* unreachable */
}

/* ------------------------------------------------------------------ */
/*  顶层 fw 命令分发                                                   */
/* ------------------------------------------------------------------ */

static int cmd_fw(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "help") == 0) {
        print_fw_usage();
        return 0;
    }
    if (strcmp(argv[1], "list") == 0) {
        return do_fw_list();
    }
    if (strcmp(argv[1], "info") == 0) {
        return do_fw_info();
    }
    if (strcmp(argv[1], "flash") == 0) {
        return do_fw_flash(argc >= 3 ? argv[2] : nullptr);
    }

    printf("Unknown subcommand: %s\n", argv[1]);
    print_fw_usage();
    return 1;
}

/* ------------------------------------------------------------------ */
/*  公开注册入口                                                       */
/* ------------------------------------------------------------------ */

void otool_esp_hosted_fw_console_print_welcome(void)
{
    OtoolEspHostedFwProvider *p = OtoolEspHostedFwDefaultFactory::provider();
    size_t count = p->firmware_count();

    printf("\n");
    printf("==========================================================\n");
    printf("  Hosted OTA Console  -  ESP32-P4 (Host) + ESP32-C6 (Co)\n");
    printf("==========================================================\n");
    printf("\n");

    printf("Commands:\n");
    printf("  fw list             List all embedded firmware images\n");
    printf("  fw info             Query co-processor firmware version\n");
    printf("  fw flash <name>     OTA-flash named image  (auto-reboots)\n");
    printf("  help                Show all registered commands\n");
    printf("\n");

    fw_console_print_runtime_summary(count);
    printf("\n");

    if (count > 0) {
        printf("Embedded firmware (%zu):\n", count);
        for (size_t i = 0; i < count; i++) {
            const char *name = p->firmware_name_at(i);
            const char *original_name = fw_original_name_at(p, i);
            otool_esp_hosted_fw_blob_t blob = {};
            size_t sz = 0;
            if (p->get_blob(name, blob) == ESP_OK) {
                sz = (size_t)(blob.end - blob.start);
            }
            printf("  %-30s  %7.1f KB\n",
                   original_name, (float)sz / 1024.0f);
            if (strcmp(original_name, name) != 0) {
                printf("  %-30s  ->  flash-%s\n", name, name);
            } else {
                printf("  %-30s  ->  flash-%s\n", name, name);
            }
        }
        printf("\n");
    }

    printf("Shortcuts:\n");
    printf("  %-20s  fw list\n", "fl");
    printf("  %-20s  fw info\n", "fi");
    for (size_t i = 0; i < s_sc_count; i++) {
        if (strcmp(s_sc_cmd_name[i] + 6, s_sc_fw_name[i]) == 0) {
            printf("  %-20s  fw flash %s\n", s_sc_cmd_name[i], s_sc_fw_name[i]);
        } else {
            printf("  %-20s  fw flash %s\n", s_sc_cmd_name[i], s_sc_fw_name[i]);
        }
    }
    printf("\n");

    printf("Examples:\n");
    printf("  fw info\n");
    printf("  fw list\n");
    if (count > 0) {
        const char *first = p->firmware_name_at(0);
        const char *first_original = fw_original_name_at(p, 0);
        printf("  fw flash %s\n", first_original);
        printf("  flash-%s\n", first);
    }
    printf("\n");
    printf("==========================================================\n");
    printf("\n");
}

esp_err_t otool_esp_hosted_fw_console_register_cmds(void)
{
    /* 注册主命令 fw */
    const esp_console_cmd_t fw_cmd = {
        .command  = "fw",
        .help     = "Firmware management: fw <list|info|flash [name]>",
        .hint     = NULL,
        .func     = cmd_fw,
        .argtable = NULL,
    };
    esp_err_t ret = esp_console_cmd_register(&fw_cmd);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register 'fw' command: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 注册 fl / fi 快捷命令 */
    const esp_console_cmd_t fl_cmd = {
        .command  = "fl",
        .help     = "Shortcut: fw list",
        .hint     = NULL,
        .func     = cmd_fl,
        .argtable = NULL,
    };
    esp_console_cmd_register(&fl_cmd);

    const esp_console_cmd_t fi_cmd = {
        .command  = "fi",
        .help     = "Shortcut: fw info",
        .hint     = NULL,
        .func     = cmd_fi,
        .argtable = NULL,
    };
    esp_console_cmd_register(&fi_cmd);

    /* 为每个嵌入固件注册 flash-<name> 快捷命令 */
    OtoolEspHostedFwProvider *p = OtoolEspHostedFwDefaultFactory::provider();
    size_t count = p->firmware_count();
    if (count > MAX_FLASH_SHORTCUTS) count = MAX_FLASH_SHORTCUTS;
    s_sc_count = count;

    for (size_t i = 0; i < count; i++) {
        const char *fn = p->firmware_name_at(i);
        strncpy(s_sc_fw_name[i], fn, sizeof(s_sc_fw_name[0]) - 1);
        snprintf(s_sc_cmd_name[i], sizeof(s_sc_cmd_name[0]), "flash-%s", fn);
        snprintf(s_sc_help[i], sizeof(s_sc_help[0]), "Shortcut: fw flash %s", fn);

        const esp_console_cmd_t sc_cmd = {
            .command  = s_sc_cmd_name[i],
            .help     = s_sc_help[i],
            .hint     = NULL,
            .func     = s_sc_funcs[i],
            .argtable = NULL,
        };
        esp_console_cmd_register(&sc_cmd);
    }

    return ESP_OK;
}

#endif /* CONFIG_OTOOL_ESP_HOSTED_FW_ENABLE_CONSOLE_CMDS */
