// otool_tab5_live2d console: interactive command line over USB-Serial-JTAG.
// Commands: help, system/network/LLM/Agent controls and cloud speech probes.

#include "llm_app.h"
#include "wifi_app.h"
#include "credential_store.h"
#include "agent_app.h"
#include "otool_speech_sdk.h"

#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linenoise/linenoise.h"
#include "nvs.h"
#include "sdkconfig.h"

#include <cstdio>
#include <cstring>
#include <atomic>
#include <cstdlib>

static const char *TAG = "console";

/* ---------------- cloud speech probes (no microphone/speaker I/O) ---------------- */

enum class speech_probe_mode_t {
    TTS_ONLY,
    TTS_TO_ASR,
};

struct speech_probe_request_t {
    speech_probe_mode_t mode;
    char text[256];
};

struct speech_probe_audio_t {
    int16_t *pcm;
    size_t capacity_bytes;
    size_t bytes;
    size_t chunks;
    uint32_t peak;
    uint64_t absolute_sum;
    uint32_t checksum;
    int64_t first_chunk_us;
};

struct speech_probe_asr_t {
    size_t updates;
    char final_text[CONFIG_OTOOL_SPEECH_ASR_MAX_TRANSCRIPT_BYTES];
};

struct speech_file_probe_request_t {
    char audio_url[384];
    char audio_format[8];
};

static std::atomic<bool> s_speech_probe_running{false};

static esp_err_t speech_probe_pcm_callback(const int16_t *samples, size_t sample_count,
                                           void *user_ctx)
{
    auto *audio = static_cast<speech_probe_audio_t *>(user_ctx);
    if (audio == nullptr || (samples == nullptr && sample_count != 0) ||
        sample_count > SIZE_MAX / sizeof(int16_t)) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t incoming_bytes = sample_count * sizeof(int16_t);
    if (audio->pcm != nullptr) {
        if (audio->bytes > audio->capacity_bytes ||
            incoming_bytes > audio->capacity_bytes - audio->bytes) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(reinterpret_cast<uint8_t *>(audio->pcm) + audio->bytes,
               samples, incoming_bytes);
    }
    if (audio->first_chunk_us == 0 && incoming_bytes != 0) {
        audio->first_chunk_us = esp_timer_get_time();
    }
    const uint8_t *raw = reinterpret_cast<const uint8_t *>(samples);
    for (size_t i = 0; i < incoming_bytes; ++i) {
        audio->checksum ^= raw[i];
        audio->checksum *= 16777619U;
    }
    for (size_t i = 0; i < sample_count; ++i) {
        int32_t value = samples[i];
        uint32_t magnitude = static_cast<uint32_t>(value < 0 ? -value : value);
        if (magnitude > audio->peak) audio->peak = magnitude;
        audio->absolute_sum += magnitude;
    }
    audio->bytes += incoming_bytes;
    ++audio->chunks;
    return ESP_OK;
}

static void speech_probe_asr_callback(const char *text, bool definite, void *user_ctx)
{
    auto *asr = static_cast<speech_probe_asr_t *>(user_ctx);
    if (asr == nullptr || text == nullptr) return;
    ++asr->updates;
    if (definite) {
        snprintf(asr->final_text, sizeof(asr->final_text), "%s", text);
    }
    printf("[speech-probe] ASR %s bytes=%u text=%s\n",
           definite ? "final" : "partial", (unsigned)strlen(text), text);
    fflush(stdout);
}

static void speech_probe_task(void *arg)
{
    auto *request = static_cast<speech_probe_request_t *>(arg);
    const bool loopback = request->mode == speech_probe_mode_t::TTS_TO_ASR;
    bool passed = false;
    int64_t started_us = esp_timer_get_time();
    int64_t tts_started_us = 0;
    speech_probe_audio_t audio{};
    audio.checksum = 2166136261U;

    printf("[speech-probe] begin mode=%s text_bytes=%u heap=%u psram=%u\n",
           loopback ? "tts-to-asr" : "tts-only", (unsigned)strlen(request->text),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    do {
        if (CONFIG_OTOOL_SPEECH_API_KEY[0] == '\0' ||
            CONFIG_OTOOL_SPEECH_TTS_RESOURCE_ID[0] == '\0' ||
            CONFIG_OTOOL_SPEECH_TTS_SPEAKER[0] == '\0') {
            printf("[speech-probe] TTS configuration missing in sdkconfig\n");
            break;
        }
        if (loopback) {
            audio.capacity_bytes = CONFIG_OTOOL_SPEECH_PROBE_MAX_PCM_BYTES;
            audio.pcm = static_cast<int16_t *>(heap_caps_malloc(
                audio.capacity_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (audio.pcm == nullptr) {
                printf("[speech-probe] PSRAM allocation failed: bytes=%u\n",
                       (unsigned)audio.capacity_bytes);
                break;
            }
        }

        otool_speech_tts_config_t tts{};
        tts.struct_size = sizeof(tts);
        tts.api_key = CONFIG_OTOOL_SPEECH_API_KEY;
        tts.resource_id = CONFIG_OTOOL_SPEECH_TTS_RESOURCE_ID;
        tts.speaker = CONFIG_OTOOL_SPEECH_TTS_SPEAKER;
        tts.sample_rate_hz = 16000;
        tts.timeout_ms = CONFIG_OTOOL_SPEECH_TTS_TIMEOUT_MS;
        tts_started_us = esp_timer_get_time();
        esp_err_t err = otool_speech_tts_stream(
            &tts, request->text, speech_probe_pcm_callback, &audio);
        int64_t tts_done_us = esp_timer_get_time();
        uint64_t sample_count = audio.bytes / sizeof(int16_t);
        uint32_t mean_abs = sample_count != 0
            ? static_cast<uint32_t>(audio.absolute_sum / sample_count) : 0;
        printf("[speech-probe] TTS result=%s chunks=%u pcm_bytes=%u duration_ms=%u "
               "first_pcm_ms=%u peak=%u mean_abs=%u checksum=%08x\n",
               esp_err_to_name(err), (unsigned)audio.chunks, (unsigned)audio.bytes,
               (unsigned)(audio.bytes * 1000ULL / (16000U * sizeof(int16_t))),
               audio.first_chunk_us != 0
                   ? (unsigned)((audio.first_chunk_us - tts_started_us) / 1000) : 0U,
               (unsigned)audio.peak, (unsigned)mean_abs, (unsigned)audio.checksum);
        if (err != ESP_OK || audio.bytes == 0 || audio.chunks == 0) break;
        if (!loopback) {
            passed = true;
            break;
        }
        if (CONFIG_OTOOL_SPEECH_ASR_RESOURCE_ID[0] == '\0') {
            printf("[speech-probe] ASR resource id missing in sdkconfig\n");
            break;
        }

        speech_probe_asr_t asr_result{};
        otool_speech_asr_config_t asr_config{};
        asr_config.struct_size = sizeof(asr_config);
        asr_config.api_key = CONFIG_OTOOL_SPEECH_API_KEY;
        asr_config.resource_id = CONFIG_OTOOL_SPEECH_ASR_RESOURCE_ID;
        asr_config.connect_timeout_ms = 15000;
        asr_config.enable_itn = true;
        asr_config.enable_punctuation = true;
        asr_config.enable_ddc = true;
        asr_config.enable_nonstream = false;
        asr_config.on_transcript = speech_probe_asr_callback;
        asr_config.user_ctx = &asr_result;
        otool_speech_asr_handle_t asr = nullptr;
        int64_t asr_started_us = esp_timer_get_time();
        err = otool_speech_asr_open(&asr_config, &asr);
        printf("[speech-probe] ASR open=%s\n", esp_err_to_name(err));
        if (err != ESP_OK) break;

        const size_t total_samples = audio.bytes / sizeof(int16_t);
        const size_t paced_samples = 16000U * 200U / 1000U;
        size_t offset = 0;
        while (offset < total_samples) {
            size_t count = total_samples - offset;
            if (count > paced_samples) count = paced_samples;
            err = otool_speech_asr_write_pcm(asr, audio.pcm + offset, count);
            if (err != ESP_OK) break;
            offset += count;
            if (offset < total_samples) vTaskDelay(pdMS_TO_TICKS(200));
        }
        if (err == ESP_OK) {
            err = otool_speech_asr_finish(asr, asr_result.final_text,
                                          sizeof(asr_result.final_text),
                                          CONFIG_OTOOL_SPEECH_ASR_FINISH_TIMEOUT_MS);
        }
        otool_speech_asr_close(asr);
        int64_t asr_done_us = esp_timer_get_time();
        printf("[speech-probe] ASR result=%s updates=%u text_bytes=%u "
               "elapsed_ms=%u exact_match=%d\n",
               esp_err_to_name(err), (unsigned)asr_result.updates,
               (unsigned)strlen(asr_result.final_text),
               (unsigned)((asr_done_us - asr_started_us) / 1000),
               strcmp(asr_result.final_text, request->text) == 0);
        passed = err == ESP_OK && asr_result.final_text[0] != '\0';
        (void)tts_done_us;
    } while (false);

    if (audio.pcm != nullptr) {
        memset(audio.pcm, 0, audio.bytes);
        heap_caps_free(audio.pcm);
    }
    printf("[speech-probe] %s total_ms=%u heap=%u psram=%u\n",
           passed ? "PASS" : "FAIL",
           (unsigned)((esp_timer_get_time() - started_us) / 1000),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    fflush(stdout);
    free(request);
    s_speech_probe_running.store(false);
    vTaskDelete(nullptr);
}

static int start_speech_probe(int argc, char **argv, speech_probe_mode_t mode)
{
    if (s_speech_probe_running.exchange(true)) {
        printf("speech-probe: another probe is running\n");
        return 1;
    }
    auto *request = static_cast<speech_probe_request_t *>(
        calloc(1, sizeof(speech_probe_request_t)));
    if (request == nullptr) {
        s_speech_probe_running.store(false);
        printf("speech-probe: request allocation failed\n");
        return 1;
    }
    request->mode = mode;
    static const char *default_text = "你好，我正在进行语音接口回环测试。";
    if (argc < 2) {
        snprintf(request->text, sizeof(request->text), "%s", default_text);
    } else {
        size_t pos = 0;
        for (int i = 1; i < argc && pos < sizeof(request->text) - 1; ++i) {
            if (i > 1 && pos < sizeof(request->text) - 2) request->text[pos++] = ' ';
            size_t count = strlen(argv[i]);
            if (count > sizeof(request->text) - 1 - pos) {
                count = sizeof(request->text) - 1 - pos;
            }
            memcpy(request->text + pos, argv[i], count);
            pos += count;
        }
        request->text[pos] = '\0';
    }
    if (xTaskCreate(speech_probe_task, "speech_probe",
                    CONFIG_OTOOL_SPEECH_PROBE_TASK_STACK_SIZE,
                    request, 5, nullptr) != pdPASS) {
        free(request);
        s_speech_probe_running.store(false);
        printf("speech-probe: task creation failed\n");
        return 1;
    }
    printf("speech-probe: queued mode=%s text_bytes=%u\n",
           mode == speech_probe_mode_t::TTS_TO_ASR ? "tts-to-asr" : "tts-only",
           (unsigned)strlen(request->text));
    return 0;
}

static int do_speech_tts_probe(int argc, char **argv)
{
    return start_speech_probe(argc, argv, speech_probe_mode_t::TTS_ONLY);
}

static int do_speech_loopback(int argc, char **argv)
{
    return start_speech_probe(argc, argv, speech_probe_mode_t::TTS_TO_ASR);
}

static void speech_asr_probe_task(void *arg)
{
    (void)arg;
    int64_t started_us = esp_timer_get_time();
    bool passed = false;
    speech_probe_asr_t asr_result{};
    printf("[speech-probe] begin mode=asr-silence heap=%u psram=%u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    do {
        if (CONFIG_OTOOL_SPEECH_API_KEY[0] == '\0' ||
            CONFIG_OTOOL_SPEECH_ASR_RESOURCE_ID[0] == '\0') {
            printf("[speech-probe] ASR configuration missing in sdkconfig\n");
            break;
        }
        otool_speech_asr_config_t config{};
        config.struct_size = sizeof(config);
        config.api_key = CONFIG_OTOOL_SPEECH_API_KEY;
        config.resource_id = CONFIG_OTOOL_SPEECH_ASR_RESOURCE_ID;
        config.connect_timeout_ms = 15000;
        config.enable_itn = true;
        config.enable_punctuation = true;
        config.enable_ddc = true;
        config.enable_nonstream = false;
        config.on_transcript = speech_probe_asr_callback;
        config.user_ctx = &asr_result;

        otool_speech_asr_handle_t asr = nullptr;
        esp_err_t err = otool_speech_asr_open(&config, &asr);
        printf("[speech-probe] ASR open=%s\n", esp_err_to_name(err));
        if (err != ESP_OK) break;

        constexpr size_t packet_samples = 16000U * 200U / 1000U;
        auto *silence = static_cast<int16_t *>(heap_caps_calloc(
            packet_samples, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (silence == nullptr) {
            silence = static_cast<int16_t *>(calloc(packet_samples, sizeof(int16_t)));
        }
        if (silence == nullptr) {
            (void)otool_speech_asr_cancel(asr);
            otool_speech_asr_close(asr);
            printf("[speech-probe] ASR silence allocation failed\n");
            break;
        }
        for (int packet = 0; packet < 5 && err == ESP_OK; ++packet) {
            err = otool_speech_asr_write_pcm(asr, silence, packet_samples);
            if (packet != 4) vTaskDelay(pdMS_TO_TICKS(200));
        }
        free(silence);
        if (err == ESP_OK) {
            err = otool_speech_asr_finish(asr, asr_result.final_text,
                                          sizeof(asr_result.final_text),
                                          CONFIG_OTOOL_SPEECH_ASR_FINISH_TIMEOUT_MS);
        }
        otool_speech_asr_close(asr);
        printf("[speech-probe] ASR silence result=%s updates=%u text_bytes=%u\n",
               esp_err_to_name(err), (unsigned)asr_result.updates,
               (unsigned)strlen(asr_result.final_text));
        passed = err == ESP_OK;
    } while (false);

    printf("[speech-probe] %s mode=asr-silence total_ms=%u heap=%u psram=%u\n",
           passed ? "PASS" : "FAIL",
           (unsigned)((esp_timer_get_time() - started_us) / 1000),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    fflush(stdout);
    s_speech_probe_running.store(false);
    vTaskDelete(nullptr);
}

static int do_speech_asr_probe(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (s_speech_probe_running.exchange(true)) {
        printf("speech-probe: another probe is running\n");
        return 1;
    }
    if (xTaskCreate(speech_asr_probe_task, "speech_asr_probe",
                    CONFIG_OTOOL_SPEECH_PROBE_TASK_STACK_SIZE,
                    nullptr, 5, nullptr) != pdPASS) {
        s_speech_probe_running.store(false);
        printf("speech-probe: ASR task creation failed\n");
        return 1;
    }
    printf("speech-probe: queued mode=asr-silence\n");
    return 0;
}

static void speech_asr_file_probe_task(void *arg)
{
    auto *request = static_cast<speech_file_probe_request_t *>(arg);
    int64_t started_us = esp_timer_get_time();
    char transcript[CONFIG_OTOOL_SPEECH_ASR_MAX_TRANSCRIPT_BYTES] = "";
    otool_speech_asr_file_result_t result{};
    otool_speech_asr_file_config_t config{};
    config.struct_size = sizeof(config);
    config.api_key = CONFIG_OTOOL_SPEECH_API_KEY;
    config.resource_id = CONFIG_OTOOL_SPEECH_ASR_RESOURCE_ID;
    config.request_timeout_ms = 15000;
    config.query_interval_ms = 500;
    config.overall_timeout_ms = 120000;
    config.enable_itn = true;
    config.enable_punctuation = true;
    config.show_utterances = true;

    printf("[speech-probe] begin mode=asr-file url_bytes=%u format=%s heap=%u psram=%u\n",
           (unsigned)strlen(request->audio_url), request->audio_format,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    esp_err_t err = otool_speech_asr_file_recognize_url(
        &config, request->audio_url, request->audio_format,
        transcript, sizeof(transcript), &result);
    printf("[speech-probe] ASR file result=%s provider=%ld message=%.96s "
           "text_bytes=%u audio_ms=%u request_id=%s logid=%s elapsed_ms=%u\n",
           esp_err_to_name(err), (long)result.provider_code,
           result.message[0] != '\0' ? result.message : "-",
           (unsigned)strlen(transcript), (unsigned)result.audio_duration_ms,
           result.request_id[0] != '\0' ? result.request_id : "-",
           result.log_id[0] != '\0' ? result.log_id : "-",
           (unsigned)((esp_timer_get_time() - started_us) / 1000));
    if (transcript[0] != '\0') {
        printf("[speech-probe] transcript=%s\n", transcript);
    }
    printf("[speech-probe] %s mode=asr-file\n",
           err == ESP_OK && transcript[0] != '\0' ? "PASS" : "FAIL");
    fflush(stdout);
    memset(request, 0, sizeof(*request));
    free(request);
    s_speech_probe_running.store(false);
    vTaskDelete(nullptr);
}

static int do_speech_asr_file_probe(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        printf("usage: speech-asr-file-probe <public-audio-url> [raw|wav|mp3|ogg]\n");
        return 1;
    }
    if (s_speech_probe_running.exchange(true)) {
        printf("speech-probe: another probe is running\n");
        return 1;
    }
    auto *request = static_cast<speech_file_probe_request_t *>(
        calloc(1, sizeof(speech_file_probe_request_t)));
    if (request == nullptr) {
        s_speech_probe_running.store(false);
        printf("speech-probe: request allocation failed\n");
        return 1;
    }
    if (strlen(argv[1]) >= sizeof(request->audio_url)) {
        free(request);
        s_speech_probe_running.store(false);
        printf("speech-probe: audio URL is too long\n");
        return 1;
    }
    snprintf(request->audio_url, sizeof(request->audio_url), "%s", argv[1]);
    const char *format = argc == 3 ? argv[2] : "wav";
    snprintf(request->audio_format, sizeof(request->audio_format), "%s", format);
    if (xTaskCreate(speech_asr_file_probe_task, "speech_asr_file",
                    CONFIG_OTOOL_SPEECH_PROBE_TASK_STACK_SIZE,
                    request, 5, nullptr) != pdPASS) {
        memset(request, 0, sizeof(*request));
        free(request);
        s_speech_probe_running.store(false);
        printf("speech-probe: file ASR task creation failed\n");
        return 1;
    }
    printf("speech-probe: queued mode=asr-file url_bytes=%u format=%s\n",
           (unsigned)strlen(request->audio_url), request->audio_format);
    return 0;
}

static int do_speech_probe_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("speech-probe: %s\n", s_speech_probe_running.load() ? "running" : "idle");
    return 0;
}

/* ---------------- wifi ---------------- */

static int do_wifi_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta == nullptr) {
        printf("wifi: sta netif not found\n");
        return 1;
    }
    esp_netif_ip_info_t ip = {};
    if (esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0) {
        printf("wifi: connected ip=" IPSTR "\n", IP2STR(&ip.ip));
    } else {
        printf("wifi: not connected\n");
    }
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        printf("wifi: ssid=%s rssi=%d channel=%d\n", (const char *)ap.ssid, ap.rssi, ap.primary);
    }
    return 0;
}

static int do_wifi_reconnect(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_err_t err = wifi_app_reconnect();
    printf("wifi: reconnect -> %s\n", esp_err_to_name(err));
    return 0;
}

/* ---------------- llm ---------------- */

static int do_llm_ask(int argc, char **argv)
{
    if (argc > 1) {
        /* llm-ask <text...>：拼接全部参数作为完整问题（支持含空格的中文/英文） */
        char question[256];
        size_t pos = 0;
        for (int i = 1; i < argc && pos < sizeof(question) - 1; i++) {
            if (i > 1 && pos < sizeof(question) - 2) {
                question[pos++] = ' ';
            }
            size_t n = strlen(argv[i]);
            if (n > sizeof(question) - 1 - pos) {
                n = sizeof(question) - 1 - pos;
            }
            memcpy(question + pos, argv[i], n);
            pos += n;
        }
        question[pos] = '\0';
        llm_app_ask_text(question);
        printf("llm: run queued (%u bytes)\n", (unsigned)strlen(question));
        return 0;
    }

    /* llm-ask（无参数）：二次确认后才使用默认问题；回车或 y 确认，其他/Ctrl+C 取消 */
    char *line = linenoise("Use default question? [y/N] (Enter/y to confirm, Ctrl+C cancels): ");
    if (line == nullptr) {
        printf("llm: cancelled (Ctrl+C)\n");
        return 0;
    }
    if (line[0] == '\0' || line[0] == 'y' || line[0] == 'Y') {
        llm_app_ask_now();
        printf("llm: asking default question\n");
    } else {
        printf("llm: cancelled\n");
    }
    return 0;
}

static int do_llm_cancel(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    llm_app_cancel_now();
    printf("llm: cancel requested\n");
    return 0;
}

static int do_llm_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    llm_app_status_t st;
    llm_app_get_status(&st);
    printf("llm: round=%d busy=%d reply_len=%u err=%s\n", st.round, (int)st.busy,
           (unsigned)st.reply_len, st.error[0] ? st.error : "-");
    return 0;
}

/* ---------------- system helpers ---------------- */

static int do_free(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("free heap: %u bytes, largest block: %u bytes\n",
           (unsigned)esp_get_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return 0;
}

static int do_version(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("otool_tab5_live2d %s\n", IDF_VER);
    return 0;
}

static int do_stack(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    static const char *names[] = { "agent_worker", "llm_worker", "lvgl_task", "repl" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        TaskHandle_t t = xTaskGetHandle(names[i]);
        if (t != nullptr) {
            printf("stack %s: hwm=%lu bytes\n", names[i],
                   (unsigned long)uxTaskGetStackHighWaterMark(t));
        }
    }
    return 0;
}

/* ---------------- cred（有效凭证；sdkconfig 优先，NVS 后备） ---------------- */

static const char *cred_valid_names[] = { "wifi_ssid", "wifi_pass", "llm_key" };

static bool cred_name_valid(const char *name)
{
    for (size_t i = 0; i < sizeof(cred_valid_names) / sizeof(cred_valid_names[0]); i++) {
        if (strcmp(name, cred_valid_names[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void cred_print_masked(const char *name, const char *value)
{
    size_t len = strlen(value);
    if (len == 0) {
        printf("  %s = <empty>\n", name);
        return;
    }
    /* SSID/密码/Key 均不显示任何前缀，短值也不能原样泄漏。 */
    printf("  %s = <set> (%u chars)\n", name, (unsigned)len);
}

static int do_cred_set(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: cred-set <name> <value>   (names: wifi_ssid|wifi_pass|llm_key)\n");
        return 1;
    }
    if (!cred_name_valid(argv[1])) {
        printf("cred: unknown name '%s' (use wifi_ssid|wifi_pass|llm_key)\n", argv[1]);
        return 1;
    }
    esp_err_t err = credential_store_set(argv[1], argv[2]);
    if (err != ESP_OK) {
        printf("cred: set failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("cred: %s saved to NVS fallback; a non-empty sdkconfig value still takes precedence\n",
           argv[1]);
    return 0;
}

static int do_cred_show(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    for (size_t i = 0; i < sizeof(cred_valid_names) / sizeof(cred_valid_names[0]); i++) {
        char buf[256];
        if (credential_store_copy_runtime(cred_valid_names[i], buf, sizeof(buf)) == ESP_OK) {
            cred_print_masked(cred_valid_names[i], buf);
        } else {
            printf("  %s = <unset>\n", cred_valid_names[i]);
        }
        volatile char *p = buf;
        for (size_t j = 0; j < sizeof(buf); j++) {
            p[j] = '\0';
        }
    }
    return 0;
}

static int do_cred_clear(int argc, char **argv)
{
    if (argc < 2 || !cred_name_valid(argv[1])) {
        printf("usage: cred-clear <name>   (names: wifi_ssid|wifi_pass|llm_key)\n");
        return 1;
    }
    esp_err_t err = credential_store_erase(argv[1]);
    if (err != ESP_OK) {
        printf("cred: clear failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("cred: %s erased from NVS fallback; sdkconfig value is unchanged\n", argv[1]);
    return 0;
}

/* ---------------- agent ---------------- */

static int do_agent_run(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: agent <text...>   (tool-enabled agent run; Ctrl+C via agent-cancel)\n");
        return 1;
    }
    char question[256];
    size_t pos = 0;
    for (int i = 1; i < argc && pos < sizeof(question) - 1; i++) {
        if (i > 1 && pos < sizeof(question) - 2) {
            question[pos++] = ' ';
        }
        size_t n = strlen(argv[i]);
        if (n > sizeof(question) - 1 - pos) {
            n = sizeof(question) - 1 - pos;
        }
        memcpy(question + pos, argv[i], n);
        pos += n;
    }
    question[pos] = '\0';
    agent_app_ask(question);
    printf("agent: run queued (%u bytes)\n", (unsigned)strlen(question));
    return 0;
}

static int do_agent_cancel(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    agent_app_cancel();
    printf("agent: cancel requested\n");
    return 0;
}

static int do_agent_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    char buf[192];
    agent_app_status(buf, sizeof(buf));
    printf("agent: %s\n", buf);
    return 0;
}

static int do_agent_reset(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    agent_app_reset_session();
    printf("agent: conversation reset queued\n");
    return 0;
}

static int do_agent_protocol(int argc, char **argv)
{
    if (argc < 2) {
        printf("agent-protocol: %s (restart to apply)\n", agent_proto_name());
        printf("usage: agent-protocol <responses|chat>\n");
        return 0;
    }
    if (strcmp(argv[1], "responses") != 0 && strcmp(argv[1], "chat") != 0) {
        printf("agent-protocol: invalid '%s' (use responses|chat)\n", argv[1]);
        return 1;
    }
    nvs_handle_t h;
    if (nvs_open("otool_cfg", NVS_READWRITE, &h) != ESP_OK) {
        printf("agent-protocol: nvs open failed\n");
        return 1;
    }
    esp_err_t err = nvs_set_str(h, "agent_proto", argv[1]);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        printf("agent-protocol: save failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("agent-protocol: set to %s (restart to apply)\n", argv[1]);
    return 0;
}

/* ---------------- init ---------------- */

static void register_commands(void)
{
    esp_console_register_help_command();

    esp_console_cmd_t cmd = {};

    cmd.command = "wifi";
    cmd.help = "wifi status";
    cmd.func = &do_wifi_status;
    esp_console_cmd_register(&cmd);

    cmd.command = "wifi-reconnect";
    cmd.help = "disconnect and reconnect STA";
    cmd.func = &do_wifi_reconnect;
    esp_console_cmd_register(&cmd);

    cmd.command = "llm-ask";
    cmd.help = "ask a question: 'llm-ask <text>'; without text, confirm default (Ctrl+C cancels)";
    cmd.func = &do_llm_ask;
    esp_console_cmd_register(&cmd);

    cmd.command = "llm-cancel";
    cmd.help = "cancel in-flight LLM request";
    cmd.func = &do_llm_cancel;
    esp_console_cmd_register(&cmd);

    cmd.command = "llm-status";
    cmd.help = "LLM worker status";
    cmd.func = &do_llm_status;
    esp_console_cmd_register(&cmd);

    cmd.command = "free";
    cmd.help = "show free heap";
    cmd.func = &do_free;
    esp_console_cmd_register(&cmd);

    cmd.command = "stack";
    cmd.help = "task stack high-water marks";
    cmd.func = &do_stack;
    esp_console_cmd_register(&cmd);

    cmd.command = "version";
    cmd.help = "show firmware version";
    cmd.func = &do_version;
    esp_console_cmd_register(&cmd);

    cmd.command = "cred";
    cmd.help = "show effective credentials, sdkconfig first (masked)";
    cmd.func = &do_cred_show;
    esp_console_cmd_register(&cmd);

    cmd.command = "cred-set";
    cmd.help = "store optional NVS fallback: cred-set <wifi_ssid|wifi_pass|llm_key> <value>";
    cmd.func = &do_cred_set;
    esp_console_cmd_register(&cmd);

    cmd.command = "cred-clear";
    cmd.help = "erase an NVS fallback: cred-clear <wifi_ssid|wifi_pass|llm_key>";
    cmd.func = &do_cred_clear;
    esp_console_cmd_register(&cmd);

    cmd.command = "agent";
    cmd.help = "tool-enabled agent run: agent <text...>";
    cmd.func = &do_agent_run;
    esp_console_cmd_register(&cmd);

    cmd.command = "agent-cancel";
    cmd.help = "cancel the active agent run";
    cmd.func = &do_agent_cancel;
    esp_console_cmd_register(&cmd);

    cmd.command = "agent-status";
    cmd.help = "agent worker status";
    cmd.func = &do_agent_status;
    esp_console_cmd_register(&cmd);

    cmd.command = "agent-reset";
    cmd.help = "cancel the active run and clear agent conversation state";
    cmd.func = &do_agent_reset;
    esp_console_cmd_register(&cmd);

    cmd.command = "agent-protocol";
    cmd.help = "agent protocol: agent-protocol <responses|chat> (restart to apply)";
    cmd.func = &do_agent_protocol;
    esp_console_cmd_register(&cmd);

    cmd.command = "speech-tts-probe";
    cmd.help = "cloud TTS/API data probe without speaker: speech-tts-probe [text]";
    cmd.func = &do_speech_tts_probe;
    esp_console_cmd_register(&cmd);

    cmd.command = "speech-loopback";
    cmd.help = "cloud TTS PCM -> ASR probe without microphone/speaker: speech-loopback [text]";
    cmd.func = &do_speech_loopback;
    esp_console_cmd_register(&cmd);

    cmd.command = "speech-asr-probe";
    cmd.help = "Streaming ASR 2.0 silence probe; requires a SAUC resource";
    cmd.func = &do_speech_asr_probe;
    esp_console_cmd_register(&cmd);

    cmd.command = "speech-asr-file-probe";
    cmd.help = "recording-file ASR submit/query probe: speech-asr-file-probe <public-url> [format]";
    cmd.func = &do_speech_asr_file_probe;
    esp_console_cmd_register(&cmd);

    cmd.command = "speech-probe-status";
    cmd.help = "show cloud speech probe state";
    cmd.func = &do_speech_probe_status;
    esp_console_cmd_register(&cmd);
}

extern "C" void console_start(void)
{
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.max_cmdline_length = 256;
    repl_cfg.prompt = "tab5> ";

    esp_console_dev_usb_serial_jtag_config_t dev_cfg = {};

    esp_console_repl_t *repl = nullptr;
    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "new repl usb_serial_jtag: %s", esp_err_to_name(err));
        return;
    }

    register_commands();

    err = esp_console_start_repl(repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start repl: %s", esp_err_to_name(err));
    }
}
