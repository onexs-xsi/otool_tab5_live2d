#include "otool_esp_hosted_fw_update_types.h"

#include <string.h>

#ifdef CONFIG_OTOOL_ESP_HOSTED_FW_PROVIDER_EMBEDDED
extern const uint8_t _binary_fw_tab5_bin_start[];
extern const uint8_t _binary_fw_tab5_bin_end[];
extern const uint8_t _binary_fw_bluedroid_bin_start[];
extern const uint8_t _binary_fw_bluedroid_bin_end[];
extern const uint8_t _binary_fw_nimble_bin_start[];
extern const uint8_t _binary_fw_nimble_bin_end[];
extern const uint8_t _binary_fw_noble_bin_start[];
extern const uint8_t _binary_fw_noble_bin_end[];
#endif

#ifdef CONFIG_OTOOL_ESP_HOSTED_FW_EMBED_NETWORK_ADAPTER
extern const uint8_t _binary_network_adapter_bin_start[];
extern const uint8_t _binary_network_adapter_bin_end[];
#endif

typedef struct {
    const char *name;
    const char *original_name;
    const uint8_t *start;
    const uint8_t *end;
} otool_esp_hosted_fw_entry_t;

static const otool_esp_hosted_fw_entry_t s_esp_hosted_fw_table[] = {
#ifdef CONFIG_OTOOL_ESP_HOSTED_FW_PROVIDER_EMBEDDED
    {"tab5",       "fw_tab5.bin",      _binary_fw_tab5_bin_start,      _binary_fw_tab5_bin_end},
    {"bluedroid",  "fw_bluedroid.bin", _binary_fw_bluedroid_bin_start, _binary_fw_bluedroid_bin_end},
    {"nimble",     "fw_nimble.bin",    _binary_fw_nimble_bin_start,    _binary_fw_nimble_bin_end},
    {"noble",      "fw_noble.bin",     _binary_fw_noble_bin_start,     _binary_fw_noble_bin_end},
#endif
#ifdef CONFIG_OTOOL_ESP_HOSTED_FW_EMBED_NETWORK_ADAPTER
    // 直接由 slave 源码构建的 app-only 固件，名称与 slave 构建产物保持一致
    {"network_adapter", "network_adapter.bin",
     _binary_network_adapter_bin_start, _binary_network_adapter_bin_end},
#endif
};

esp_err_t OtoolEspHostedFwEmbeddedProvider::get_blob(const char *firmware_name, otool_esp_hosted_fw_blob_t &blob)
{
    if (firmware_name == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < sizeof(s_esp_hosted_fw_table) / sizeof(s_esp_hosted_fw_table[0]); i++) {
        if (strcmp(firmware_name, s_esp_hosted_fw_table[i].name) == 0 || strcmp(firmware_name, s_esp_hosted_fw_table[i].original_name) == 0) {
            blob.name = s_esp_hosted_fw_table[i].name;
            blob.start = s_esp_hosted_fw_table[i].start;
            blob.end = s_esp_hosted_fw_table[i].end;
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

size_t OtoolEspHostedFwEmbeddedProvider::firmware_count() const
{
    return sizeof(s_esp_hosted_fw_table) / sizeof(s_esp_hosted_fw_table[0]);
}

const char *OtoolEspHostedFwEmbeddedProvider::firmware_name_at(size_t index) const
{
    const size_t count = sizeof(s_esp_hosted_fw_table) / sizeof(s_esp_hosted_fw_table[0]);
    if (index >= count) {
        return nullptr;
    }
    return s_esp_hosted_fw_table[index].name;
}

const char *OtoolEspHostedFwEmbeddedProvider::firmware_original_name_at(size_t index) const
{
    const size_t count = sizeof(s_esp_hosted_fw_table) / sizeof(s_esp_hosted_fw_table[0]);
    if (index >= count) {
        return nullptr;
    }
    return s_esp_hosted_fw_table[index].original_name;
}

