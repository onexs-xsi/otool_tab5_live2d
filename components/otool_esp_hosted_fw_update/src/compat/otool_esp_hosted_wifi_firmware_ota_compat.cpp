#include "otool_esp_hosted_fw_update.h"

extern "C" esp_err_t otool_esp_hosted_wifi_firmware_ota(const char *firmware_name)
{
    return otool_esp_hosted_fw_update_by_name(firmware_name);
}

