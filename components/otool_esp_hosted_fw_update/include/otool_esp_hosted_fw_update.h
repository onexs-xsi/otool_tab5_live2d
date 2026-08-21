#pragma once

#include "esp_err.h"
#include "otool_esp_hosted_fw_update_types.h"

class OtoolEspHostedFwUpdater {
public:
    OtoolEspHostedFwUpdater(OtoolEspHostedFwProvider *provider, OtoolEspHostedFwTransport *transport);
    esp_err_t update_by_name(const char *firmware_name);
    const char *version() const;

private:
    OtoolEspHostedFwProvider *provider_;
    OtoolEspHostedFwTransport *transport_;
};

class OtoolEspHostedFwDefaultFactory {
public:
    static OtoolEspHostedFwProvider *provider();
    static OtoolEspHostedFwTransport *transport();
    static OtoolEspHostedFwUpdater *updater();
};

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t otool_esp_hosted_fw_update_by_name(const char *firmware_name);

esp_err_t otool_esp_hosted_fw_update_minimal_init(void);

const char *otool_esp_hosted_fw_update_version(void);

#ifdef __cplusplus
}
#endif

