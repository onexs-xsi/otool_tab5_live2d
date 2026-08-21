#include "otool_esp_hosted_fw_update_types.h"

extern "C" {
#include "esp_hosted_ota.h"
}

esp_err_t OtoolEspHostedFwStubTransport::begin()
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t OtoolEspHostedFwStubTransport::write(const uint8_t *data, size_t len)
{
    (void) data;
    (void) len;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t OtoolEspHostedFwStubTransport::end()
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t OtoolEspHostedFwStubTransport::activate()
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t OtoolEspHostedFwEspHostedTransport::begin()
{
    return esp_hosted_slave_ota_begin();
}

esp_err_t OtoolEspHostedFwEspHostedTransport::write(const uint8_t *data, size_t len)
{
    return esp_hosted_slave_ota_write((uint8_t *) data, len);
}

esp_err_t OtoolEspHostedFwEspHostedTransport::end()
{
    return esp_hosted_slave_ota_end();
}

esp_err_t OtoolEspHostedFwEspHostedTransport::activate()
{
    return esp_hosted_slave_ota_activate();
}

