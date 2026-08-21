#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 向 esp_console 注册 "fw" 命令组及快捷命令。
 *
 * 子命令：
 *   fw list           - 列出所有已嵌入的固件镜像及其大小
 *   fw info           - 查询协处理器当前运行的固件版本
 *   fw flash <name>   - OTA 烧录指定名称的固件，成功后重启整板
 *
 * 同时注册快捷命令：
 *   fl                - 等同于 fw list
 *   fi                - 等同于 fw info
 *   flash-<name>      - 等同于 fw flash <name>，每个嵌入固件对应一条
 *
 * 须在 esp_console_start_repl() 之前调用。
 * 须在 Kconfig 中启用 CONFIG_OTOOL_ESP_HOSTED_FW_ENABLE_CONSOLE_CMDS。
 */
esp_err_t otool_esp_hosted_fw_console_register_cmds(void);

/**
 * 打印启动欢迎信息：命令用法、嵌入固件列表、快捷命令和示例。
 * 须在 otool_esp_hosted_fw_console_register_cmds() 之后调用。
 */
void otool_esp_hosted_fw_console_print_welcome(void);

#ifdef __cplusplus
}
#endif
