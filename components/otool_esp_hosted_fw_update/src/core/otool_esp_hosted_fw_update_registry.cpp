#include "otool_esp_hosted_fw_update_registry.h"

#include "esp_err.h"

static OtoolEspHostedFwStubProvider s_stub_provider;
#if CONFIG_OTOOL_ESP_HOSTED_FW_PROVIDER_EMBEDDED
static OtoolEspHostedFwEmbeddedProvider s_embedded_provider;
#endif
static OtoolEspHostedFwStubTransport s_stub_transport;
static OtoolEspHostedFwEspHostedTransport s_esp_hosted_transport;

OtoolEspHostedFwProvider *otool_esp_hosted_fw_registry_provider(void)
{
#if CONFIG_OTOOL_ESP_HOSTED_FW_PROVIDER_EMBEDDED
    return &s_embedded_provider;
#else
    return &s_stub_provider;
#endif
}

OtoolEspHostedFwTransport *otool_esp_hosted_fw_registry_transport(void)
{
#if CONFIG_OTOOL_ESP_HOSTED_FW_TRANSPORT_ESP_HOSTED
    return &s_esp_hosted_transport;
#else
    return &s_stub_transport;
#endif
}

