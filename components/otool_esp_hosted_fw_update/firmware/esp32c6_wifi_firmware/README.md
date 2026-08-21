# Firmware Binaries for ESP32-C6 Slave

This directory holds the binary file(s) embedded into the Host firmware at build time.

---

## 两种嵌入模式

### 模式 A：多变体预编译包（`OTOOL_ESP_HOSTED_FW_PROVIDER_EMBEDDED`）

| 文件名 | 说明 |
|---|---|
| `fw_bluedroid.bin` | Bluedroid BT 栈（Classic BT + BLE） |
| `fw_nimble.bin` | NimBLE BT 栈（BLE only） |
| `fw_noble.bin` | 无 BLE 构建 |
| `fw_tab5.bin` | M5Stack Tab5 专用配置 |

这些文件来自预编译分发包，版本可能与本仓库 slave 源码不一致。

---

### 模式 B：app-only（`OTOOL_ESP_HOSTED_FW_EMBED_NETWORK_ADAPTER`）[推荐]

只需要一个文件：`network_adapter.bin`，直接由本仓库 slave 源码构建，与代码一一对应。

**构建步骤（ESP32-C6 + SDIO）：**

```bash
cd example/esp-hosted-mcu-main/slave

# 首次构建需要设置目标芯片
idf.py set-target esp32c6

# 如需使用非默认 BT 配置（默认为 NimBLE），可在此编辑 sdkconfig.defaults.esp32c6
# 例如：
#   CONFIG_BT_BLUEDROID_ENABLED=y
#   CONFIG_BT_NIMBLE_ENABLED=n

idf.py build

# 复制构建产物到本目录
cp build/network_adapter.bin \
   components/otool_esp_hosted_fw_update/firmware/esp32c6_wifi_firmware/network_adapter.bin
```

复制完成后，在 `idf.py menuconfig` 中启用：
```
Otool ESP Hosted FW Update
  → [x] Embed single network_adapter.bin (app-only, built from slave source)
```

Host 固件使用 `"network_adapter"` 作为固件名称调用 OTA API，例如：
```cpp
updater.update_by_name("network_adapter");
```

---

## 版本追溯建议

建议在 git commit message 或版本文件中记录：
- slave 源码的 git commit hash
- 使用的 IDF 版本
- 目标芯片和 BT 栈配置

