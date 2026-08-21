# otool_esp_hosted_fw_update

ESP-IDF component skeleton for ESP32-C6 firmware OTA over `esp_hosted`.

## Current status

- Skeleton is ready for multi-platform extension.
- ESP-IDF + `esp_hosted` transport path is prepared.
- Embedded firmware provider is optional and disabled by default.
- Compatibility API `otool_esp_hosted_wifi_firmware_ota()` is optional and disabled by default.

## Kconfig options

- `CONFIG_OTOOL_ESP_HOSTED_FW_CHUNK_SIZE`
- `CONFIG_OTOOL_ESP_HOSTED_FW_TRANSPORT_ESP_HOSTED`
- `CONFIG_OTOOL_ESP_HOSTED_FW_PROVIDER_EMBEDDED`
- `CONFIG_OTOOL_ESP_HOSTED_FW_ENABLE_COMPAT_API`

## Notes

- Runtime logs keep ASCII only.
- If provider is not enabled, `otool_esp_hosted_fw_update_by_name()` returns `ESP_ERR_NOT_FOUND`.

