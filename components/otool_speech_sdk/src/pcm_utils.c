#include "otool_speech_sdk.h"

size_t otool_speech_pcm_4ch_to_mono(const int16_t *input, size_t input_samples,
                                    uint8_t selected_channel, int16_t *output,
                                    size_t output_capacity_samples)
{
    if (input == NULL || output == NULL || selected_channel >= 4) {
        return 0;
    }
    size_t frames = input_samples / 4;
    if (frames > output_capacity_samples) {
        frames = output_capacity_samples;
    }
    for (size_t i = 0; i < frames; ++i) {
        output[i] = input[i * 4 + selected_channel];
    }
    return frames;
}

size_t otool_speech_pcm_mono_to_stereo(const int16_t *input, size_t input_samples,
                                       int16_t *output, size_t output_capacity_samples)
{
    if (input == NULL || output == NULL) {
        return 0;
    }
    size_t frames = input_samples;
    if (frames > output_capacity_samples / 2) {
        frames = output_capacity_samples / 2;
    }
    for (size_t i = 0; i < frames; ++i) {
        output[i * 2] = input[i];
        output[i * 2 + 1] = input[i];
    }
    return frames;
}
