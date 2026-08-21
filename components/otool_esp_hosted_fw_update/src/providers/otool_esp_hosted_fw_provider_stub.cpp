#include "otool_esp_hosted_fw_update_types.h"

esp_err_t OtoolEspHostedFwStubProvider::get_blob(const char *firmware_name, otool_esp_hosted_fw_blob_t &blob)
{
    (void) firmware_name;
    (void) blob;
    return ESP_ERR_NOT_FOUND;
}

