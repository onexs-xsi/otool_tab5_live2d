#ifndef OTOOL_SPEECH_GZIP_CODEC_H
#define OTOOL_SPEECH_GZIP_CODEC_H

#include "esp_err.h"

#include <stddef.h>
#include <stdint.h>

typedef struct otool_speech_gzip_encoder otool_speech_gzip_encoder_t;
typedef struct otool_speech_gzip_decoder otool_speech_gzip_decoder_t;

otool_speech_gzip_encoder_t *otool_speech_gzip_encoder_create(void);
void otool_speech_gzip_encoder_destroy(otool_speech_gzip_encoder_t *encoder);
otool_speech_gzip_decoder_t *otool_speech_gzip_decoder_create(void);
void otool_speech_gzip_decoder_destroy(otool_speech_gzip_decoder_t *decoder);

size_t otool_speech_gzip_bound(size_t input_size);
esp_err_t otool_speech_gzip_compress(otool_speech_gzip_encoder_t *encoder,
                                     const uint8_t *input, size_t input_size,
                                     uint8_t *output, size_t output_capacity,
                                     size_t *output_size);
esp_err_t otool_speech_gzip_decompress(otool_speech_gzip_decoder_t *decoder,
                                       const uint8_t *input, size_t input_size,
                                       uint8_t *output, size_t output_capacity,
                                       size_t *output_size);

#endif
