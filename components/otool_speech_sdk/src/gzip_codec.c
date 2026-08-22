#include "gzip_codec.h"
#include "speech_memory.h"

#include "miniz.h"

#include <stdlib.h>
#include <string.h>

struct otool_speech_gzip_encoder {
    tdefl_compressor compressor;
};

struct otool_speech_gzip_decoder {
    tinfl_decompressor decompressor;
};

otool_speech_gzip_encoder_t *otool_speech_gzip_encoder_create(void)
{
    return otool_speech_alloc_large(sizeof(otool_speech_gzip_encoder_t));
}

void otool_speech_gzip_encoder_destroy(otool_speech_gzip_encoder_t *encoder)
{
    free(encoder);
}

otool_speech_gzip_decoder_t *otool_speech_gzip_decoder_create(void)
{
    return otool_speech_alloc_large(sizeof(otool_speech_gzip_decoder_t));
}

void otool_speech_gzip_decoder_destroy(otool_speech_gzip_decoder_t *decoder)
{
    free(decoder);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void write_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

size_t otool_speech_gzip_bound(size_t input_size)
{
    /* miniz's raw-deflate bound plus the 10-byte gzip header and 8-byte
     * trailer. The small fixed margin also covers tiny incompressible input. */
    return input_size + input_size / 8 + 96;
}

esp_err_t otool_speech_gzip_compress(otool_speech_gzip_encoder_t *encoder,
                                     const uint8_t *input, size_t input_size,
                                     uint8_t *output, size_t output_capacity,
                                     size_t *output_size)
{
    if (encoder == NULL || (input == NULL && input_size != 0) ||
        output == NULL || output_size == NULL ||
        output_capacity < 18 || input_size > UINT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    static const uint8_t header[10] = {
        0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff,
    };
    memcpy(output, header, sizeof(header));

    tdefl_status status = tdefl_init(&encoder->compressor, NULL, NULL,
                                     1 | TDEFL_GREEDY_PARSING_FLAG);
    if (status != TDEFL_STATUS_OKAY) return ESP_FAIL;
    size_t consumed = input_size;
    size_t deflated_size = output_capacity - sizeof(header) - 8;
    status = tdefl_compress(&encoder->compressor, input, &consumed,
                            output + sizeof(header), &deflated_size,
                            TDEFL_FINISH);
    if (status != TDEFL_STATUS_DONE || consumed != input_size ||
        deflated_size == 0 || deflated_size > output_capacity - 18) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t *footer = output + sizeof(header) + deflated_size;
    uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, input, input_size);
    write_le32(footer, crc);
    write_le32(footer + 4, (uint32_t)input_size);
    *output_size = sizeof(header) + deflated_size + 8;
    return ESP_OK;
}

static esp_err_t gzip_payload(const uint8_t *input, size_t input_size,
                              const uint8_t **payload, size_t *payload_size)
{
    if (input_size < 18 || input[0] != 0x1f || input[1] != 0x8b || input[2] != 8) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint8_t flags = input[3];
    if ((flags & 0xe0) != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    size_t pos = 10;
    size_t footer_pos = input_size - 8;
    if ((flags & 0x04) != 0) {
        if (pos + 2 > footer_pos) return ESP_ERR_INVALID_RESPONSE;
        size_t extra = (size_t)input[pos] | ((size_t)input[pos + 1] << 8);
        pos += 2;
        if (extra > footer_pos - pos) return ESP_ERR_INVALID_RESPONSE;
        pos += extra;
    }
    if ((flags & 0x08) != 0) {
        while (pos < footer_pos && input[pos++] != 0) {}
        if (pos == footer_pos && input[pos - 1] != 0) return ESP_ERR_INVALID_RESPONSE;
    }
    if ((flags & 0x10) != 0) {
        while (pos < footer_pos && input[pos++] != 0) {}
        if (pos == footer_pos && input[pos - 1] != 0) return ESP_ERR_INVALID_RESPONSE;
    }
    if ((flags & 0x02) != 0) {
        if (pos + 2 > footer_pos) return ESP_ERR_INVALID_RESPONSE;
        pos += 2;
    }
    if (pos > footer_pos) return ESP_ERR_INVALID_RESPONSE;
    *payload = input + pos;
    *payload_size = footer_pos - pos;
    return ESP_OK;
}

esp_err_t otool_speech_gzip_decompress(otool_speech_gzip_decoder_t *decoder,
                                       const uint8_t *input, size_t input_size,
                                       uint8_t *output, size_t output_capacity,
                                       size_t *output_size)
{
    if (decoder == NULL || input == NULL || output == NULL || output_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t *payload = NULL;
    size_t payload_size = 0;
    esp_err_t err = gzip_payload(input, input_size, &payload, &payload_size);
    if (err != ESP_OK) return err;

    uint32_t expected_size = read_le32(input + input_size - 4);
    if (expected_size > output_capacity || payload_size > UINT32_MAX) {
        return ESP_ERR_NO_MEM;
    }

    tinfl_init(&decoder->decompressor);
    size_t consumed = payload_size;
    size_t actual_size = output_capacity;
    tinfl_status status = tinfl_decompress(
        &decoder->decompressor, payload, &consumed, output, output,
        &actual_size, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (status != TINFL_STATUS_DONE || consumed != payload_size ||
        actual_size != expected_size) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint32_t expected_crc = read_le32(input + input_size - 8);
    uint32_t actual_crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, output, actual_size);
    if (actual_crc != expected_crc) {
        return ESP_ERR_INVALID_CRC;
    }
    *output_size = actual_size;
    return ESP_OK;
}
