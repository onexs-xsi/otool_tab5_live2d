#include "otool_esp_hosted_fw_update.h"

#include "otool_esp_hosted_fw_update_config.h"
#include "otool_esp_hosted_fw_update_registry.h"

extern "C" {
#include "esp_hosted.h"
}

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <atomic>
#include <stdio.h>
#include <string.h>

static const char *TAG = "otool_esp_hosted_fw_update";
static constexpr uint8_t OTOOL_ESP_IMAGE_MAGIC = 0xE9u;
static constexpr size_t OTOOL_ESP_IMAGE_COMMON_HEADER_LEN = 8u;
static constexpr size_t OTOOL_ESP_IMAGE_EXTENDED_HEADER_LEN = 16u;
static constexpr size_t OTOOL_ESP_IMAGE_MIN_HEADER_LEN =
    OTOOL_ESP_IMAGE_COMMON_HEADER_LEN + OTOOL_ESP_IMAGE_EXTENDED_HEADER_LEN;
static constexpr size_t OTOOL_ESP_APP_DESC_SCAN_LIMIT = 256u * 1024u;
static constexpr uint16_t OTOOL_ESP_HOSTED_OTA_TARGET_CHIP_ID = 13u;  // ESP32-C6
static constexpr size_t OTOOL_ESP_HOSTED_FW_VERSION_TEXT_LEN = 33u;
static constexpr int OTOOL_ESP_HOSTED_FW_ACTIVATE_CONFIRM_RETRIES = 12;
static constexpr int OTOOL_ESP_HOSTED_FW_ACTIVATE_CONFIRM_DELAY_MS = 500;
static const uint8_t OTOOL_ESP_APP_DESC_MAGIC[4] = {0x32, 0x54, 0xCD, 0xAB};
static std::atomic_flag s_otool_esp_hosted_fw_update_in_progress = ATOMIC_FLAG_INIT;

typedef struct __attribute__((packed)) {
    uint32_t magic_word;
    uint32_t secure_version;
    uint32_t reserv1[2];
    char     version[32];
    char     project_name[32];
    char     time_str[16];
    char     date_str[16];
    char     idf_ver[32];
    uint8_t  app_elf_sha256[32];
} otool_esp_app_desc_raw_t;

class OtoolEspHostedFwUpdateGuard {
public:
    OtoolEspHostedFwUpdateGuard()
        : locked_(!s_otool_esp_hosted_fw_update_in_progress.test_and_set(std::memory_order_acquire))
    {
    }

    ~OtoolEspHostedFwUpdateGuard()
    {
        if (locked_) {
            s_otool_esp_hosted_fw_update_in_progress.clear(std::memory_order_release);
        }
    }

    bool locked() const
    {
        return locked_;
    }

private:
    bool locked_;
};

typedef struct {
    bool valid;
    uint16_t chip_id;
    char app_version[OTOOL_ESP_HOSTED_FW_VERSION_TEXT_LEN];
} otool_esp_hosted_fw_image_info_t;

static const char *otool_esp_hosted_fw_chip_name(uint16_t chip_id)
{
    switch (chip_id) {
        case 0:  return "ESP32";
        case 2:  return "ESP32-S2";
        case 5:  return "ESP32-C3";
        case 9:  return "ESP32-S3";
        case 12: return "ESP32-C2";
        case 13: return "ESP32-C6";
        case 16: return "ESP32-H2";
        case 18: return "ESP32-P4";
        case 20: return "ESP32-C61";
        case 23: return "ESP32-C5";
        case 25: return "ESP32-H21";
        case 28: return "ESP32-H4";
        case 31: return "ESP32-E22";
        case 32: return "ESP32-S31";
        default: return "Unknown";
    }
}

static void otool_esp_hosted_fw_safe_copy_str(char *dst, size_t dst_size, const char *src, size_t src_size)
{
    if (dst == nullptr || dst_size == 0) {
        return;
    }

    size_t n = src_size;
    if (n > dst_size - 1) {
        n = dst_size - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';

    for (int i = (int) n - 1; i >= 0 && (unsigned char) dst[i] < 0x20u; --i) {
        dst[i] = '\0';
    }
}

static ptrdiff_t otool_esp_hosted_fw_find_app_desc(const uint8_t *data, size_t size)
{
    if (data == nullptr) {
        return -1;
    }

    size_t scan_limit = size;
    if (scan_limit > OTOOL_ESP_APP_DESC_SCAN_LIMIT) {
        scan_limit = OTOOL_ESP_APP_DESC_SCAN_LIMIT;
    }
    if (scan_limit < sizeof(OTOOL_ESP_APP_DESC_MAGIC)) {
        return -1;
    }

    for (size_t i = 0; i + sizeof(OTOOL_ESP_APP_DESC_MAGIC) <= scan_limit; ++i) {
        if (memcmp(data + i, OTOOL_ESP_APP_DESC_MAGIC, sizeof(OTOOL_ESP_APP_DESC_MAGIC)) == 0) {
            return (ptrdiff_t) i;
        }
    }

    return -1;
}

static bool otool_esp_hosted_fw_parse_version_string(const char *text, esp_hosted_coprocessor_fwver_t *out)
{
    if (text == nullptr || text[0] == '\0' || out == nullptr) {
        return false;
    }

    unsigned int major = 0;
    unsigned int minor = 0;
    unsigned int patch = 0;
    char tail = '\0';
    if (sscanf(text, "%u.%u.%u%c", &major, &minor, &patch, &tail) != 3) {
        return false;
    }

    *out = {};
    out->major1 = major;
    out->minor1 = minor;
    out->patch1 = patch;
    return true;
}

static bool otool_esp_hosted_fw_same_version(const esp_hosted_coprocessor_fwver_t *lhs,
                                             const esp_hosted_coprocessor_fwver_t *rhs)
{
    if (lhs == nullptr || rhs == nullptr) {
        return false;
    }

    return lhs->major1 == rhs->major1 &&
           lhs->minor1 == rhs->minor1 &&
           lhs->patch1 == rhs->patch1;
}

static bool otool_esp_hosted_fw_confirm_activate_success(const char *blob_name, const char *target_version)
{
    const char *name = (blob_name != nullptr) ? blob_name : "<unnamed>";
    esp_hosted_coprocessor_fwver_t expected = {};
    if (!otool_esp_hosted_fw_parse_version_string(target_version, &expected)) {
        ESP_LOGW(TAG, "Activate confirm skipped for '%s': target version is unknown", name);
        return false;
    }

    ESP_LOGW(TAG, "Activate response timed out for '%s'; waiting for co-processor to reboot into %s",
             name,
             target_version);

    for (int attempt = 0; attempt < OTOOL_ESP_HOSTED_FW_ACTIVATE_CONFIRM_RETRIES; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(OTOOL_ESP_HOSTED_FW_ACTIVATE_CONFIRM_DELAY_MS));

        esp_hosted_coprocessor_fwver_t actual = {};
        esp_err_t query_ret = static_cast<esp_err_t>(esp_hosted_get_coprocessor_fwversion(&actual));
        if (query_ret != ESP_OK) {
            continue;
        }

        if (otool_esp_hosted_fw_same_version(&actual, &expected)) {
            ESP_LOGW(TAG, "Activate confirmed for '%s': co-processor is now running %u.%u.%u",
                     name,
                     (unsigned int) actual.major1,
                     (unsigned int) actual.minor1,
                     (unsigned int) actual.patch1);
            return true;
        }
    }

    ESP_LOGW(TAG, "Activate confirm failed for '%s': target version %s was not observed",
             name,
             target_version);
    return false;
}

static esp_err_t otool_esp_hosted_fw_validate_image(const char *blob_name,
                                                    const uint8_t *fw_start,
                                                    size_t fw_size,
                                                    otool_esp_hosted_fw_image_info_t *image_info)
{
    const char *name = (blob_name != nullptr) ? blob_name : "<unnamed>";
    if (image_info != nullptr) {
        *image_info = {};
    }

    if (fw_start == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (fw_size < OTOOL_ESP_IMAGE_MIN_HEADER_LEN) {
        ESP_LOGE(TAG, "Reject OTA image '%s': too small (%u bytes)", name, (unsigned int) fw_size);
        return ESP_ERR_INVALID_SIZE;
    }
    if (fw_start[0] != OTOOL_ESP_IMAGE_MAGIC) {
        ESP_LOGE(TAG, "Reject OTA image '%s': bad magic 0x%02x at app start", name, fw_start[0]);
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t seg_count = fw_start[1];
    const uint16_t chip_id = (uint16_t) ((uint16_t) fw_start[12] | ((uint16_t) fw_start[13] << 8));

    if (chip_id == 0xFFFFu || seg_count <= 1u) {
        ESP_LOGE(TAG,
                 "Reject OTA image '%s': not an application image (seg_count=%u chip=%s)",
                 name,
                 (unsigned int) seg_count,
                 otool_esp_hosted_fw_chip_name(chip_id));
        return ESP_ERR_INVALID_ARG;
    }
    if (chip_id != OTOOL_ESP_HOSTED_OTA_TARGET_CHIP_ID) {
        ESP_LOGE(TAG,
                 "Reject OTA image '%s': chip=%s (%u), expected ESP32-C6",
                 name,
                 otool_esp_hosted_fw_chip_name(chip_id),
                 (unsigned int) chip_id);
        return ESP_ERR_INVALID_ARG;
    }

    const ptrdiff_t app_desc_offset = otool_esp_hosted_fw_find_app_desc(fw_start, fw_size);
    if (app_desc_offset < 0) {
        ESP_LOGE(TAG, "Reject OTA image '%s': esp_app_desc_t not found in first %u bytes",
                 name,
                 (unsigned int) ((fw_size < OTOOL_ESP_APP_DESC_SCAN_LIMIT) ? fw_size : OTOOL_ESP_APP_DESC_SCAN_LIMIT));
        return ESP_ERR_INVALID_ARG;
    }
    if ((size_t) app_desc_offset + sizeof(otool_esp_app_desc_raw_t) > fw_size) {
        ESP_LOGE(TAG, "Reject OTA image '%s': truncated esp_app_desc_t at offset %u",
                 name,
                 (unsigned int) app_desc_offset);
        return ESP_ERR_INVALID_SIZE;
    }

    otool_esp_app_desc_raw_t app_desc = {};
    memcpy(&app_desc, fw_start + app_desc_offset, sizeof(app_desc));
    char project_name[sizeof(app_desc.project_name) + 1] = {};
    char app_version[sizeof(app_desc.version) + 1] = {};
    char idf_ver[sizeof(app_desc.idf_ver) + 1] = {};

    otool_esp_hosted_fw_safe_copy_str(project_name, sizeof(project_name),
                                      app_desc.project_name, sizeof(app_desc.project_name));
    otool_esp_hosted_fw_safe_copy_str(app_version, sizeof(app_version),
                                      app_desc.version, sizeof(app_desc.version));
    otool_esp_hosted_fw_safe_copy_str(idf_ver, sizeof(idf_ver),
                                      app_desc.idf_ver, sizeof(app_desc.idf_ver));
    if (image_info != nullptr) {
        image_info->valid = true;
        image_info->chip_id = chip_id;
        otool_esp_hosted_fw_safe_copy_str(image_info->app_version, sizeof(image_info->app_version),
                                          app_desc.version, sizeof(app_desc.version));
    }

    ESP_LOGI(TAG,
             "Validated OTA image '%s': chip=%s project='%s' version='%s' idf='%s'",
             name,
             otool_esp_hosted_fw_chip_name(chip_id),
             project_name,
             app_version,
             idf_ver);
    return ESP_OK;
}

static bool otool_esp_hosted_fw_activate_supported(const esp_hosted_coprocessor_fwver_t &slave_version)
{
    return (slave_version.major1 > 2U) ||
           (slave_version.major1 == 2U && slave_version.minor1 > 5U);
}

static esp_err_t otool_esp_hosted_fw_check_version_compatibility(esp_hosted_coprocessor_fwver_t *slave_version)
{
    if (slave_version == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *slave_version = {};
    esp_err_t ret = static_cast<esp_err_t>(esp_hosted_get_coprocessor_fwversion(slave_version));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read co-processor version: %s", esp_err_to_name(ret));
        return ret;
    }

    const uint32_t host_version = ESP_HOSTED_VERSION_VAL(
        ESP_HOSTED_VERSION_MAJOR_1,
        ESP_HOSTED_VERSION_MINOR_1,
        ESP_HOSTED_VERSION_PATCH_1);
    const uint32_t slave_version_value = ESP_HOSTED_VERSION_VAL(
        slave_version->major1,
        slave_version->minor1,
        slave_version->patch1);
    const uint32_t host_major_minor = host_version & 0xFFFFFF00U;
    const uint32_t slave_major_minor = slave_version_value & 0xFFFFFF00U;

    ESP_LOGI(TAG, "Host firmware version: " ESP_HOSTED_VERSION_PRINTF_FMT,
             ESP_HOSTED_VERSION_PRINTF_ARGS(host_version));
    ESP_LOGI(TAG, "Co-processor version: %u.%u.%u",
             static_cast<unsigned int>(slave_version->major1),
             static_cast<unsigned int>(slave_version->minor1),
             static_cast<unsigned int>(slave_version->patch1));

    if (host_major_minor > slave_major_minor) {
#if CONFIG_OTOOL_ESP_HOSTED_FW_FAIL_ON_HOST_NEWER_MAJOR_MINOR
        ESP_LOGE(TAG, "Host major.minor is newer than co-processor; aborting OTA because strict compatibility check is enabled");
        return ESP_ERR_INVALID_STATE;
#else
        ESP_LOGW(TAG, "Host major.minor is newer than co-processor; OTA may hang during esp_hosted_slave_ota_begin or later RPC steps");
        ESP_LOGW(TAG, "Continuing because version mismatch is configured as warning-only");
#endif
    }

    return ESP_OK;
}

static void otool_esp_hosted_fw_adjust_image(const otool_esp_hosted_fw_blob_t *blob, const uint8_t **fw_start, size_t *fw_size)
{
    *fw_start = blob->start;
    *fw_size = (size_t) (blob->end - blob->start);

    if (*fw_size > 0x10000 && (*fw_start)[0] == 0xE9 && (*fw_start)[0x8000] == 0xAA && (*fw_start)[0x8001] == 0x50) {
        ESP_LOGW(TAG, "Detected merged binary, skip first 64KB");
        *fw_start += 0x10000;
        *fw_size -= 0x10000;
    }
}

OtoolEspHostedFwUpdater::OtoolEspHostedFwUpdater(OtoolEspHostedFwProvider *provider, OtoolEspHostedFwTransport *transport)
    : provider_(provider), transport_(transport)
{
}

esp_err_t OtoolEspHostedFwUpdater::update_by_name(const char *firmware_name)
{
    if (provider_ == nullptr || transport_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    OtoolEspHostedFwUpdateGuard update_guard;
    if (!update_guard.locked()) {
        ESP_LOGW(TAG, "Reject OTA request: another update is already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    if (firmware_name == nullptr || firmware_name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    otool_esp_hosted_fw_blob_t blob = {};
    esp_err_t ret = provider_->get_blob(firmware_name, blob);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Firmware not found: %s", firmware_name);
        return ret;
    }

    const uint8_t *fw_start = nullptr;
    size_t fw_size = 0;
    otool_esp_hosted_fw_image_info_t image_info = {};
    esp_hosted_coprocessor_fwver_t slave_version = {};
    otool_esp_hosted_fw_adjust_image(&blob, &fw_start, &fw_size);

    if (fw_size == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    ret = otool_esp_hosted_fw_validate_image(blob.name, fw_start, fw_size, &image_info);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = otool_esp_hosted_fw_check_version_compatibility(&slave_version);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "Start OTA: %s, size=%u", blob.name, (unsigned int) fw_size);

    ESP_LOGI(TAG, "Calling transport begin...");
    ret = transport_->begin();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "transport begin failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "transport begin OK, writing %u bytes in %u-byte chunks",
             (unsigned int) fw_size,
             (unsigned int) CONFIG_OTOOL_ESP_HOSTED_FW_CHUNK_SIZE);

    size_t offset = 0;
    uint32_t chunk_count = 0;
    while (offset < fw_size) {
        size_t bytes_to_send = (fw_size - offset > CONFIG_OTOOL_ESP_HOSTED_FW_CHUNK_SIZE) ? CONFIG_OTOOL_ESP_HOSTED_FW_CHUNK_SIZE : (fw_size - offset);
        ret = transport_->write(fw_start + offset, bytes_to_send);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "transport write failed at offset %u: %s", (unsigned int) offset, esp_err_to_name(ret));
            (void) transport_->end();
            return ret;
        }
        offset += bytes_to_send;
        chunk_count++;
        if ((chunk_count % 10U) == 0U) {
            ESP_LOGI(TAG, "Progress: %u/%u bytes (%u%%)",
                     (unsigned int) offset, (unsigned int) fw_size,
                     (unsigned int) (offset * 100U / fw_size));
        }
    }

    ESP_LOGI(TAG, "Write done, calling transport end...");
    ret = transport_->end();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "transport end failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (!otool_esp_hosted_fw_activate_supported(slave_version)) {
        ESP_LOGW(TAG, "Skipping transport activate: co-processor firmware %u.%u.%u does not support OTA activate RPC (requires v2.6.0+)",
                 static_cast<unsigned int>(slave_version.major1),
                 static_cast<unsigned int>(slave_version.minor1),
                 static_cast<unsigned int>(slave_version.patch1));
        ESP_LOGW(TAG, "OTA image transfer completed; reboot the co-processor or the board to let the new firmware take effect if supported by the slave boot flow");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Calling transport activate...");
    ret = transport_->activate();
    if (ret != ESP_OK && image_info.app_version[0] != '\0' &&
        otool_esp_hosted_fw_confirm_activate_success(blob.name, image_info.app_version)) {
        ESP_LOGW(TAG, "Activate returned %s for '%s', but target firmware is already running",
                 esp_err_to_name(ret),
                 blob.name);
        return ESP_OK;
    }
    return ret;
}

const char *OtoolEspHostedFwUpdater::version() const
{
    return "0.1.0-skeleton";
}

OtoolEspHostedFwProvider *OtoolEspHostedFwDefaultFactory::provider()
{
    return otool_esp_hosted_fw_registry_provider();
}

OtoolEspHostedFwTransport *OtoolEspHostedFwDefaultFactory::transport()
{
    return otool_esp_hosted_fw_registry_transport();
}

OtoolEspHostedFwUpdater *OtoolEspHostedFwDefaultFactory::updater()
{
    static OtoolEspHostedFwUpdater updater(provider(), transport());
    return &updater;
}

extern "C" esp_err_t otool_esp_hosted_fw_update_by_name(const char *firmware_name)
{
    return OtoolEspHostedFwDefaultFactory::updater()->update_by_name(firmware_name);
}

extern "C" const char *otool_esp_hosted_fw_update_version(void)
{
    return OtoolEspHostedFwDefaultFactory::updater()->version();
}

