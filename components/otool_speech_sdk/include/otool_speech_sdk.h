#ifndef OTOOL_SPEECH_SDK_H
#define OTOOL_SPEECH_SDK_H

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OTOOL_SPEECH_ASR_DEFAULT_ENDPOINT \
    "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async"
#define OTOOL_SPEECH_ASR_FILE_SUBMIT_DEFAULT_ENDPOINT \
    "https://openspeech.bytedance.com/api/v3/auc/bigmodel/submit"
#define OTOOL_SPEECH_ASR_FILE_QUERY_DEFAULT_ENDPOINT \
    "https://openspeech.bytedance.com/api/v3/auc/bigmodel/query"
#define OTOOL_SPEECH_TTS_DEFAULT_ENDPOINT \
    "wss://openspeech.bytedance.com/api/v3/tts/bidirection"

typedef struct otool_speech_asr_session *otool_speech_asr_handle_t;

/** A partial or final transcript. The text span is valid only in the callback. */
typedef void (*otool_speech_transcript_cb_t)(const char *text, bool definite, void *user_ctx);

typedef struct {
    size_t struct_size;
    const char *api_key;
    const char *resource_id;
    const char *endpoint; /* NULL selects OTOOL_SPEECH_ASR_DEFAULT_ENDPOINT. */
    uint32_t connect_timeout_ms;
    bool enable_itn;
    bool enable_punctuation;
    bool enable_ddc;
    bool enable_nonstream;
    otool_speech_transcript_cb_t on_transcript;
    void *user_ctx;
} otool_speech_asr_config_t;

/**
 * Open a Volcengine optimized bidirectional ASR session and send its initial
 * request. Input format is fixed to signed little-endian PCM, 16 kHz, mono,
 * 16-bit. Missing credentials return ESP_ERR_INVALID_STATE without starting a
 * network task.
 */
esp_err_t otool_speech_asr_open(const otool_speech_asr_config_t *config,
                                otool_speech_asr_handle_t *out_session);

/** Feed mono PCM. The SDK coalesces input into 100-200 ms protocol packets. */
esp_err_t otool_speech_asr_write_pcm(otool_speech_asr_handle_t session,
                                     const int16_t *samples, size_t sample_count);

/** Send the final audio packet and wait for the final transcript. */
esp_err_t otool_speech_asr_finish(otool_speech_asr_handle_t session,
                                  char *transcript, size_t transcript_capacity,
                                  uint32_t timeout_ms);

/** Abort transport activity. Safe to call before close. */
esp_err_t otool_speech_asr_cancel(otool_speech_asr_handle_t session);

/** Stop/destroy the WebSocket client and release all session memory. */
void otool_speech_asr_close(otool_speech_asr_handle_t session);

/** Provider result metadata for Volcengine recording-file recognition. */
typedef struct {
    int32_t provider_code;
    uint32_t audio_duration_ms;
    char request_id[37];
    char log_id[96];
    char message[128];
} otool_speech_asr_file_result_t;

typedef struct {
    size_t struct_size;
    const char *api_key;
    const char *resource_id; /* Recording-file 2.0: volc.seedasr.auc. */
    const char *submit_endpoint; /* NULL selects the default endpoint. */
    const char *query_endpoint;  /* NULL selects the default endpoint. */
    const char *language;        /* NULL/empty enables automatic language selection. */
    uint32_t request_timeout_ms;
    uint32_t query_interval_ms;
    uint32_t overall_timeout_ms;
    bool enable_itn;
    bool enable_punctuation;
    bool show_utterances;
} otool_speech_asr_file_config_t;

/**
 * Submit one complete recording by public URL, poll until completion, and
 * return the final transcript. Volcengine fetches the URL from its servers;
 * private LAN URLs and in-memory PCM cannot be used with this API.
 */
esp_err_t otool_speech_asr_file_recognize_url(
    const otool_speech_asr_file_config_t *config,
    const char *audio_url,
    const char *audio_format,
    char *transcript,
    size_t transcript_capacity,
    otool_speech_asr_file_result_t *result);

/** Callback for decoded signed little-endian PCM, 16-bit mono chunks. */
typedef esp_err_t (*otool_speech_pcm_cb_t)(const int16_t *samples, size_t sample_count,
                                           void *user_ctx);

typedef struct {
    size_t struct_size;
    const char *api_key;
    const char *resource_id;
    const char *speaker;
    const char *endpoint; /* NULL selects OTOOL_SPEECH_TTS_DEFAULT_ENDPOINT. */
    uint32_t sample_rate_hz;
    int speech_rate;
    int loudness_rate;
    uint32_t timeout_ms;
} otool_speech_tts_config_t;

/**
 * Synchronously synthesize one text with Volcengine bidirectional WebSocket
 * TTS 2.0. The SDK requests raw PCM and invokes on_pcm for each audio frame.
 */
esp_err_t otool_speech_tts_stream(const otool_speech_tts_config_t *config,
                                  const char *text,
                                  otool_speech_pcm_cb_t on_pcm,
                                  void *user_ctx);

/** Select one channel from interleaved 4-channel capture into mono. */
size_t otool_speech_pcm_4ch_to_mono(const int16_t *input, size_t input_samples,
                                    uint8_t selected_channel, int16_t *output,
                                    size_t output_capacity_samples);

/** Duplicate mono samples to interleaved stereo playback. */
size_t otool_speech_pcm_mono_to_stereo(const int16_t *input, size_t input_samples,
                                       int16_t *output, size_t output_capacity_samples);

#ifdef __cplusplus
}
#endif

#endif
