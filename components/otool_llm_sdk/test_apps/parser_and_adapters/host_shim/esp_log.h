/*
 * SPDX-FileCopyrightText: 2026 otool project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-side shim for esp_log.h 鈥?routes ESP_LOGx to a no-op stub.
 */

#ifndef HOST_SHIM_ESP_LOG_H
#define HOST_SHIM_ESP_LOG_H

void otool_llm_host_log_printf(const char *tag, const char *fmt, ...);

#define ESP_LOGE(tag, fmt, ...) otool_llm_host_log_printf(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) otool_llm_host_log_printf(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) otool_llm_host_log_printf(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) otool_llm_host_log_printf(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) otool_llm_host_log_printf(tag, fmt, ##__VA_ARGS__)

#endif /* HOST_SHIM_ESP_LOG_H */
