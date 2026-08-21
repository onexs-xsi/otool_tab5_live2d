#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name;
    const uint8_t *start;
    const uint8_t *end;
} otool_esp_hosted_fw_blob_t;

class OtoolEspHostedFwProvider {
public:
    virtual ~OtoolEspHostedFwProvider() = default;
    virtual esp_err_t get_blob(const char *firmware_name, otool_esp_hosted_fw_blob_t &blob) = 0;
    // 返回当前 provider 持有的固件条目数量（默认 0）
    virtual size_t firmware_count() const { return 0; }
    // 返回第 index 个固件的 name（超界返回 nullptr）
    virtual const char *firmware_name_at(size_t index) const { (void)index; return nullptr; }
    // 返回第 index 个固件的原始文件名；若无单独信息则退化为 firmware_name_at()
    virtual const char *firmware_original_name_at(size_t index) const { return firmware_name_at(index); }
};

class OtoolEspHostedFwTransport {
public:
    virtual ~OtoolEspHostedFwTransport() = default;
    virtual esp_err_t begin() = 0;
    virtual esp_err_t write(const uint8_t *data, size_t len) = 0;
    virtual esp_err_t end() = 0;
    virtual esp_err_t activate() = 0;
};

class OtoolEspHostedFwStubProvider final : public OtoolEspHostedFwProvider {
public:
    esp_err_t get_blob(const char *firmware_name, otool_esp_hosted_fw_blob_t &blob) override;
};

class OtoolEspHostedFwEmbeddedProvider final : public OtoolEspHostedFwProvider {
public:
    esp_err_t get_blob(const char *firmware_name, otool_esp_hosted_fw_blob_t &blob) override;
    size_t firmware_count() const override;
    const char *firmware_name_at(size_t index) const override;
    const char *firmware_original_name_at(size_t index) const override;
};

class OtoolEspHostedFwStubTransport final : public OtoolEspHostedFwTransport {
public:
    esp_err_t begin() override;
    esp_err_t write(const uint8_t *data, size_t len) override;
    esp_err_t end() override;
    esp_err_t activate() override;
};

class OtoolEspHostedFwEspHostedTransport final : public OtoolEspHostedFwTransport {
public:
    esp_err_t begin() override;
    esp_err_t write(const uint8_t *data, size_t len) override;
    esp_err_t end() override;
    esp_err_t activate() override;
};

