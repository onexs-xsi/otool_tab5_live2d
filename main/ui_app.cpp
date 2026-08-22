// OTool Tab5 UI: professional 2:1 Agent workspace with a stateful 3D companion,
// scroll-aware streaming output and an interactive ES7210 capture surface.

#include "ui_app.h"
#include "agent_app.h"
#include "otool_speech_sdk.h"
#include "otool_tab5_component.h"

#include "otool_lvgl_port.h"
#include "lvgl.h"
#include "src/font/binfont_loader/lv_binfont_loader.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#if !LV_FONT_FMT_TXT_LARGE
#error "noto_zh_mid_24.bin exceeds the 20-bit LVGL glyph bitmap index; enable LV_FONT_FMT_TXT_LARGE"
#endif

static const char *TAG = "ui_app";

static constexpr int32_t SCREEN_W = 1280;
static constexpr int32_t SCREEN_H = 720;
static constexpr int32_t PANEL_Y = 18;
static constexpr int32_t PANEL_H = 684;
static constexpr int32_t LEFT_X = 20;
static constexpr int32_t LEFT_W = 820;
static constexpr int32_t RIGHT_X = 856;
static constexpr int32_t RIGHT_W = 404;

static constexpr uint32_t CHARACTER_W = 400;
static constexpr uint32_t CHARACTER_H = 533;
static constexpr uint32_t CHARACTER_STRIDE = CHARACTER_W * 4;
static constexpr uint32_t CHARACTER_BYTES = CHARACTER_STRIDE * CHARACTER_H;

enum ui_character_state_t {
    UI_CHARACTER_IDLE = 0,
    UI_CHARACTER_LISTENING,
    UI_CHARACTER_THINKING,
    UI_CHARACTER_TOOL,
    UI_CHARACTER_SPEAKING,
    UI_CHARACTER_SUCCESS,
    UI_CHARACTER_ERROR,
    UI_CHARACTER_COUNT,
};

enum ui_audio_state_t {
    UI_AUDIO_INITIALIZING = 0,
    UI_AUDIO_READY,
    UI_AUDIO_STARTING,
    UI_AUDIO_RECORDING,
    UI_AUDIO_STOPPING,
    UI_AUDIO_TRANSCRIBING,
    UI_AUDIO_WAITING_AGENT,
    UI_AUDIO_SYNTHESIZING,
    UI_AUDIO_PLAYING,
    UI_AUDIO_SPEECH_DISABLED,
    UI_AUDIO_UNAVAILABLE,
    UI_AUDIO_ERROR,
};

static constexpr uint32_t AUDIO_SAMPLE_RATE_HZ = 16000;
static constexpr uint32_t AUDIO_CAPTURE_FRAMES = AUDIO_SAMPLE_RATE_HZ / 100;  // 10 ms
static constexpr size_t AUDIO_CAPTURE_SAMPLES =
    AUDIO_CAPTURE_FRAMES * m5::tab5::M5TAB5_AUDIO_RECORD_CHANNELS;
static constexpr size_t AUDIO_PLAYBACK_CHUNK_FRAMES = 2048;

/* CMake EMBED_FILES uses each asset basename to construct linker symbols. */
extern const uint8_t _binary_noto_zh_mid_24_bin_start[];
extern const uint8_t _binary_noto_zh_mid_24_bin_end[];
extern const uint8_t _binary_agent_state_idle_argb8888_bin_start[];
extern const uint8_t _binary_agent_state_idle_argb8888_bin_end[];
extern const uint8_t _binary_agent_state_listening_argb8888_bin_start[];
extern const uint8_t _binary_agent_state_listening_argb8888_bin_end[];
extern const uint8_t _binary_agent_state_thinking_argb8888_bin_start[];
extern const uint8_t _binary_agent_state_thinking_argb8888_bin_end[];
extern const uint8_t _binary_agent_state_tool_argb8888_bin_start[];
extern const uint8_t _binary_agent_state_tool_argb8888_bin_end[];
extern const uint8_t _binary_agent_state_speaking_argb8888_bin_start[];
extern const uint8_t _binary_agent_state_speaking_argb8888_bin_end[];
extern const uint8_t _binary_agent_state_success_argb8888_bin_start[];
extern const uint8_t _binary_agent_state_success_argb8888_bin_end[];
extern const uint8_t _binary_agent_state_error_argb8888_bin_start[];
extern const uint8_t _binary_agent_state_error_argb8888_bin_end[];

struct embedded_asset_t {
    const uint8_t *start;
    const uint8_t *end;
    const char *name;
};

static const embedded_asset_t CHARACTER_ASSETS[UI_CHARACTER_COUNT] = {
    {_binary_agent_state_idle_argb8888_bin_start,
     _binary_agent_state_idle_argb8888_bin_end, "idle"},
    {_binary_agent_state_listening_argb8888_bin_start,
     _binary_agent_state_listening_argb8888_bin_end, "listening"},
    {_binary_agent_state_thinking_argb8888_bin_start,
     _binary_agent_state_thinking_argb8888_bin_end, "thinking"},
    {_binary_agent_state_tool_argb8888_bin_start,
     _binary_agent_state_tool_argb8888_bin_end, "tool"},
    {_binary_agent_state_speaking_argb8888_bin_start,
     _binary_agent_state_speaking_argb8888_bin_end, "speaking"},
    {_binary_agent_state_success_argb8888_bin_start,
     _binary_agent_state_success_argb8888_bin_end, "success"},
    {_binary_agent_state_error_argb8888_bin_start,
     _binary_agent_state_error_argb8888_bin_end, "error"},
};

static lv_font_t *s_font_24 = nullptr;
static lv_image_dsc_t s_character_dsc[UI_CHARACTER_COUNT] = {};

static lv_obj_t *s_status_label = nullptr;
static lv_obj_t *s_status_dot = nullptr;
static lv_obj_t *s_status_pill = nullptr;
static lv_obj_t *s_status_action_label = nullptr;
static lv_obj_t *s_reply_label = nullptr;
static lv_obj_t *s_chat_scroll = nullptr;
static lv_obj_t *s_follow_button = nullptr;
static lv_obj_t *s_follow_label = nullptr;
static lv_obj_t *s_heap_label = nullptr;
static lv_obj_t *s_psram_label = nullptr;
static lv_obj_t *s_uptime_label = nullptr;
static lv_obj_t *s_character_image = nullptr;
static lv_obj_t *s_character_state_label = nullptr;
static lv_obj_t *s_character_caption = nullptr;
static lv_obj_t *s_thinking_spinner = nullptr;
static lv_obj_t *s_thinking_hint = nullptr;
static lv_obj_t *s_record_button = nullptr;
static lv_obj_t *s_record_icon = nullptr;
static lv_obj_t *s_record_state_label = nullptr;
static lv_obj_t *s_transcript_title = nullptr;
static lv_obj_t *s_transcript_text = nullptr;
static lv_obj_t *s_wave_bars[5] = {};

static m5::tab5::otool_tab5_component *s_tab5 = nullptr;
static SemaphoreHandle_t s_audio_command_sem = nullptr;
static SemaphoreHandle_t s_speech_text_lock = nullptr;
static TaskHandle_t s_audio_task = nullptr;
static std::atomic<ui_audio_state_t> s_audio_state{UI_AUDIO_INITIALIZING};
static std::atomic<bool> s_record_requested{false};
static std::atomic<bool> s_audio_retry_requested{false};
static std::atomic<uint32_t> s_audio_level_percent{0};
static std::atomic<uint32_t> s_recorded_bytes{0};
static std::atomic<uint32_t> s_capture_started_ms{0};
static std::atomic<esp_err_t> s_audio_error{ESP_OK};
alignas(4) static int16_t s_capture_buffer[AUDIO_CAPTURE_SAMPLES] = {};
alignas(4) static int16_t s_capture_mono[AUDIO_CAPTURE_FRAMES] = {};
alignas(4) static int16_t s_playback_stereo[AUDIO_PLAYBACK_CHUNK_FRAMES * 2] = {};
static char s_live_transcript[CONFIG_OTOOL_SPEECH_ASR_MAX_TRANSCRIPT_BYTES] = "";
static char s_speech_notice[160] = "";
static char s_final_transcript[CONFIG_OTOOL_SPEECH_ASR_MAX_TRANSCRIPT_BYTES] = "";
static char s_tts_reply[4096] = "";

static char s_status_text[96] = "正在启动";
static char s_reply_text[4096 + 160] = "";
static agent_app_phase_t s_last_phase = static_cast<agent_app_phase_t>(-1);
static ui_audio_state_t s_last_audio_state = static_cast<ui_audio_state_t>(-1);
static uint32_t s_last_metric_second = UINT32_MAX;
static bool s_auto_follow = true;
static bool s_programmatic_scroll = false;
static bool s_last_capture_visual = false;
static uint32_t s_reset_feedback_until_ms = 0;

static void make_non_interactive(lv_obj_t *obj)
{
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static void style_card(lv_obj_t *obj, lv_color_t color, int32_t radius)
{
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
}

static lv_obj_t *decor_circle(lv_obj_t *parent, int32_t x, int32_t y, int32_t size,
                              uint32_t color, lv_opa_t opa)
{
    lv_obj_t *circle = lv_obj_create(parent);
    lv_obj_set_pos(circle, x, y);
    lv_obj_set_size(circle, size, size);
    style_card(circle, lv_color_hex(color), LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(circle, opa, 0);
    make_non_interactive(circle);
    return circle;
}

static lv_obj_t *metric_label_create(lv_obj_t *parent, int32_t x, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, 31);
    lv_obj_set_width(label, 96);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xd9d7f0), 0);
    lv_obj_set_style_text_line_space(label, 3, 0);
    return label;
}

static bool agent_phase_is_busy(agent_app_phase_t phase)
{
    return phase == AGENT_APP_PHASE_THINKING || phase == AGENT_APP_PHASE_TOOL ||
           phase == AGENT_APP_PHASE_RESPONDING;
}

static bool capture_is_active(ui_audio_state_t state)
{
    return state == UI_AUDIO_STARTING || state == UI_AUDIO_RECORDING ||
           state == UI_AUDIO_STOPPING || state == UI_AUDIO_TRANSCRIBING;
}

static bool speech_key_configured(void)
{
    return CONFIG_OTOOL_SPEECH_API_KEY[0] != '\0' &&
           CONFIG_OTOOL_SPEECH_ASR_RESOURCE_ID[0] != '\0';
}

static void speech_text_set(const char *transcript, const char *notice)
{
    if (s_speech_text_lock == nullptr ||
        xSemaphoreTake(s_speech_text_lock, portMAX_DELAY) != pdTRUE) return;
    if (transcript != nullptr) {
        snprintf(s_live_transcript, sizeof(s_live_transcript), "%s", transcript);
    }
    if (notice != nullptr) {
        snprintf(s_speech_notice, sizeof(s_speech_notice), "%s", notice);
    }
    xSemaphoreGive(s_speech_text_lock);
}

static void speech_text_copy(char *transcript, size_t transcript_capacity,
                             char *notice, size_t notice_capacity)
{
    if (transcript_capacity != 0) transcript[0] = '\0';
    if (notice_capacity != 0) notice[0] = '\0';
    if (s_speech_text_lock == nullptr ||
        xSemaphoreTake(s_speech_text_lock, portMAX_DELAY) != pdTRUE) return;
    if (transcript_capacity != 0) {
        snprintf(transcript, transcript_capacity, "%s", s_live_transcript);
    }
    if (notice_capacity != 0) {
        snprintf(notice, notice_capacity, "%s", s_speech_notice);
    }
    xSemaphoreGive(s_speech_text_lock);
}

static void audio_level_update(const int16_t *samples, size_t count)
{
    uint32_t peak = 0;
    for (size_t i = 0; i < count; ++i) {
        int32_t value = samples[i];
        uint32_t magnitude = static_cast<uint32_t>(value < 0 ? -value : value);
        if (magnitude > peak) peak = magnitude;
    }
    uint32_t instantaneous = peak >= 20480 ? 100 : (peak * 100U / 20480U);
    uint32_t previous = s_audio_level_percent.load();
    s_audio_level_percent.store((previous * 3U + instantaneous) / 4U);
}

static esp_err_t audio_hardware_init(void)
{
    if (s_tab5 == nullptr) return ESP_ERR_INVALID_STATE;

    m5::tab5::m5tab5_audio_config_t config{};
    config.sample_rate_hz = AUDIO_SAMPLE_RATE_HZ;
    config.dma_desc_num = 8;
    config.dma_frame_num = AUDIO_CAPTURE_FRAMES;
    config.output_volume = 60;
    config.input_gain_db = 30.0f;
    config.auto_enable_speaker = true;
    return s_tab5->audio_init(config);
}

static void asr_transcript_callback(const char *text, bool definite, void *user_ctx)
{
    (void)user_ctx;
    speech_text_set(text, definite ? "识别完成" : "正在实时识别");
}

struct tts_playback_context_t {
    bool started;
};

static esp_err_t tts_pcm_callback(const int16_t *samples, size_t sample_count, void *user_ctx)
{
    auto *context = static_cast<tts_playback_context_t *>(user_ctx);
    if (s_tab5 == nullptr || context == nullptr) return ESP_ERR_INVALID_STATE;
    if (!context->started) {
        esp_err_t err = s_tab5->audio_play_start();
        if (err != ESP_OK) return err;
        context->started = true;
        s_audio_state.store(UI_AUDIO_PLAYING);
        speech_text_set(nullptr, "正在播放 Agent 回复");
    }

    size_t offset = 0;
    while (offset < sample_count) {
        size_t frames = sample_count - offset;
        if (frames > AUDIO_PLAYBACK_CHUNK_FRAMES) frames = AUDIO_PLAYBACK_CHUNK_FRAMES;
        size_t converted = otool_speech_pcm_mono_to_stereo(
            samples + offset, frames, s_playback_stereo,
            sizeof(s_playback_stereo) / sizeof(s_playback_stereo[0]));
        if (converted != frames) return ESP_ERR_INVALID_SIZE;
        size_t expected_bytes = converted * 2 * sizeof(int16_t);
        size_t written = 0;
        esp_err_t err = s_tab5->audio_play_write(s_playback_stereo, expected_bytes,
                                                 &written, 2000);
        if (err != ESP_OK || written != expected_bytes) {
            return err != ESP_OK ? err : ESP_FAIL;
        }
        offset += converted;
    }
    return ESP_OK;
}

static esp_err_t wait_for_agent_reply(int previous_round, uint32_t timeout_ms)
{
    int64_t deadline_us = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
    bool run_started = false;
    while (esp_timer_get_time() < deadline_us) {
        if (agent_app_round() > previous_round) run_started = true;
        agent_app_phase_t phase = agent_app_phase();
        if (phase == AGENT_APP_PHASE_DISABLED) return ESP_ERR_INVALID_STATE;
        if (run_started && !agent_app_busy()) {
            if (phase == AGENT_APP_PHASE_COMPLETED) return ESP_OK;
            if (phase == AGENT_APP_PHASE_CANCELLED) return ESP_ERR_INVALID_STATE;
            if (phase == AGENT_APP_PHASE_ERROR) return ESP_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return ESP_ERR_TIMEOUT;
}

static void audio_capture_task(void *arg)
{
    (void)arg;
    bool initialized = false;

    for (;;) {
        if (!initialized) {
            s_audio_state.store(UI_AUDIO_INITIALIZING);
            esp_err_t err = audio_hardware_init();
            s_audio_error.store(err);
            if (err == ESP_OK) {
                initialized = true;
                s_audio_state.store(speech_key_configured()
                                        ? UI_AUDIO_READY : UI_AUDIO_SPEECH_DISABLED);
                if (!speech_key_configured()) {
                    speech_text_set("", "语音服务未配置：请在 sdkconfig 设置 Speech API Key");
                }
                ESP_LOGI(TAG, "audio interaction ready");
            } else {
                s_audio_state.store(UI_AUDIO_UNAVAILABLE);
                ESP_LOGW(TAG, "audio init unavailable: %s", esp_err_to_name(err));
            }
        }

        xSemaphoreTake(s_audio_command_sem, portMAX_DELAY);

        if (s_audio_retry_requested.exchange(false)) {
            s_record_requested.store(false);
            if (s_tab5 != nullptr) (void)s_tab5->audio_deinit();
            initialized = false;
            continue;
        }
        if (!initialized || !s_record_requested.load()) {
            continue;
        }

        s_audio_state.store(UI_AUDIO_STARTING);
        s_audio_level_percent.store(0);
        s_recorded_bytes.store(0);
        s_capture_started_ms.store(static_cast<uint32_t>(esp_timer_get_time() / 1000));
        speech_text_set("", "正在连接火山引擎流式识别");

        otool_speech_asr_config_t asr_config{};
        asr_config.struct_size = sizeof(asr_config);
        asr_config.api_key = CONFIG_OTOOL_SPEECH_API_KEY;
        asr_config.resource_id = CONFIG_OTOOL_SPEECH_ASR_RESOURCE_ID;
        asr_config.connect_timeout_ms = 15000;
        asr_config.enable_itn = true;
        asr_config.enable_punctuation = true;
        asr_config.enable_ddc = true;
        asr_config.enable_nonstream = false;
        asr_config.on_transcript = asr_transcript_callback;
        otool_speech_asr_handle_t asr = nullptr;
        esp_err_t err = otool_speech_asr_open(&asr_config, &asr);
        if (err != ESP_OK) {
            s_record_requested.store(false);
            s_audio_error.store(err);
            s_audio_state.store(UI_AUDIO_ERROR);
            speech_text_set(nullptr, "语音识别连接失败；轻触按钮重试");
            ESP_LOGE(TAG, "ASR open failed: %s", esp_err_to_name(err));
            continue;
        }

        err = s_tab5->audio_record_start();
        s_audio_error.store(err);
        if (err != ESP_OK) {
            otool_speech_asr_close(asr);
            s_record_requested.store(false);
            s_audio_state.store(UI_AUDIO_ERROR);
            speech_text_set(nullptr, "麦克风启动失败；轻触按钮重试");
            ESP_LOGE(TAG, "record start failed: %s", esp_err_to_name(err));
            continue;
        }

        s_audio_state.store(UI_AUDIO_RECORDING);
        speech_text_set(nullptr, "录音中；正在实时识别");
        esp_err_t stream_error = ESP_OK;
        while (s_record_requested.load()) {
            size_t bytes_read = 0;
            err = s_tab5->audio_record_read(
                s_capture_buffer,
                sizeof(s_capture_buffer),
                &bytes_read,
                100);
            if (bytes_read > 0) {
                s_recorded_bytes.fetch_add(static_cast<uint32_t>(bytes_read));
                size_t mono_samples = otool_speech_pcm_4ch_to_mono(
                    s_capture_buffer, bytes_read / sizeof(int16_t),
                    CONFIG_OTOOL_SPEECH_MIC_CHANNEL, s_capture_mono,
                    sizeof(s_capture_mono) / sizeof(s_capture_mono[0]));
                audio_level_update(s_capture_mono, mono_samples);
                stream_error = otool_speech_asr_write_pcm(asr, s_capture_mono, mono_samples);
                if (stream_error != ESP_OK) {
                    s_audio_error.store(stream_error);
                    s_record_requested.store(false);
                    ESP_LOGE(TAG, "ASR audio send failed: %s", esp_err_to_name(stream_error));
                    break;
                }
            }
            if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
                s_audio_error.store(err);
                s_record_requested.store(false);
                ESP_LOGE(TAG, "record read failed: %s", esp_err_to_name(err));
                break;
            }
        }

        s_audio_state.store(UI_AUDIO_STOPPING);
        esp_err_t stop_err = s_tab5->audio_record_stop();
        if (stream_error != ESP_OK || (err != ESP_OK && err != ESP_ERR_TIMEOUT)) {
            (void)otool_speech_asr_cancel(asr);
            otool_speech_asr_close(asr);
            s_audio_state.store(UI_AUDIO_ERROR);
            speech_text_set(nullptr, "录音或识别流中断；轻触按钮重试");
        } else if (stop_err != ESP_OK) {
            (void)otool_speech_asr_cancel(asr);
            otool_speech_asr_close(asr);
            s_audio_error.store(stop_err);
            s_audio_state.store(UI_AUDIO_ERROR);
            speech_text_set(nullptr, "麦克风停止失败；轻触按钮重试");
            ESP_LOGE(TAG, "record stop failed: %s", esp_err_to_name(stop_err));
        } else {
            s_audio_state.store(UI_AUDIO_TRANSCRIBING);
            speech_text_set(nullptr, "正在等待最终识别结果");
            esp_err_t asr_error = otool_speech_asr_finish(
                asr, s_final_transcript, sizeof(s_final_transcript),
                CONFIG_OTOOL_SPEECH_ASR_FINISH_TIMEOUT_MS);
            otool_speech_asr_close(asr);
            if (asr_error != ESP_OK || s_final_transcript[0] == '\0') {
                s_audio_error.store(asr_error != ESP_OK ? asr_error : ESP_ERR_INVALID_RESPONSE);
                s_audio_state.store(UI_AUDIO_ERROR);
                speech_text_set(nullptr, "没有获得有效识别结果；轻触按钮重试");
                continue;
            }

            speech_text_set(s_final_transcript, "识别完成，已发送给 Agent");
            s_audio_state.store(UI_AUDIO_WAITING_AGENT);
            int previous_round = agent_app_round();
            agent_app_ask(s_final_transcript);
            esp_err_t agent_error = wait_for_agent_reply(previous_round, 130000);
            if (agent_error != ESP_OK) {
                s_audio_error.store(agent_error);
                s_audio_state.store(UI_AUDIO_ERROR);
                speech_text_set(nullptr,
                    agent_app_phase() == AGENT_APP_PHASE_DISABLED
                        ? "识别成功，但文本 Agent 未配置 API Key"
                        : "识别成功，但 Agent 未能完成回复");
                continue;
            }

            size_t reply_size = agent_app_reply_read(s_tts_reply, sizeof(s_tts_reply));
            if (reply_size == 0) {
                s_audio_error.store(ESP_ERR_INVALID_RESPONSE);
                s_audio_state.store(UI_AUDIO_ERROR);
                speech_text_set(nullptr, "Agent 已完成，但没有可播放的文本");
                continue;
            }
            if (CONFIG_OTOOL_SPEECH_TTS_SPEAKER[0] == '\0') {
                s_audio_error.store(ESP_OK);
                s_audio_level_percent.store(0);
                s_audio_state.store(UI_AUDIO_READY);
                speech_text_set(nullptr, "Agent 已回复；未配置 TTS 音色，已跳过播放");
                continue;
            }

            s_audio_state.store(UI_AUDIO_SYNTHESIZING);
            speech_text_set(nullptr, "正在合成 Agent 回复");
            otool_speech_tts_config_t tts_config{};
            tts_config.struct_size = sizeof(tts_config);
            tts_config.api_key = CONFIG_OTOOL_SPEECH_API_KEY;
            tts_config.resource_id = CONFIG_OTOOL_SPEECH_TTS_RESOURCE_ID;
            tts_config.speaker = CONFIG_OTOOL_SPEECH_TTS_SPEAKER;
            tts_config.sample_rate_hz = AUDIO_SAMPLE_RATE_HZ;
            tts_config.timeout_ms = CONFIG_OTOOL_SPEECH_TTS_TIMEOUT_MS;
            tts_playback_context_t playback{};
            esp_err_t tts_error = otool_speech_tts_stream(
                &tts_config, s_tts_reply, tts_pcm_callback, &playback);
            if (playback.started) (void)s_tab5->audio_play_stop();
            if (tts_error != ESP_OK || !playback.started) {
                s_audio_error.store(tts_error != ESP_OK ? tts_error : ESP_ERR_INVALID_RESPONSE);
                s_audio_state.store(UI_AUDIO_ERROR);
                speech_text_set(nullptr, "Agent 已回复，但语音合成或播放失败");
                continue;
            }

            s_audio_error.store(ESP_OK);
            s_audio_level_percent.store(0);
            s_audio_state.store(UI_AUDIO_READY);
            speech_text_set(nullptr, "回复播放完成；可以开始下一轮");
        }
    }
}

static void scroll_chat_to_bottom(lv_anim_enable_t animation)
{
    if (s_chat_scroll == nullptr) return;
    s_programmatic_scroll = true;
    lv_obj_scroll_to_y(s_chat_scroll, LV_COORD_MAX, animation);
    s_programmatic_scroll = false;
}

static void follow_button_refresh(void)
{
    if (s_follow_label == nullptr || s_follow_button == nullptr) return;
    lv_label_set_text(s_follow_label, s_auto_follow ? "FOLLOW" : "PAUSED");
    lv_obj_set_style_bg_color(
        s_follow_button,
        lv_color_hex(s_auto_follow ? 0xe9f8f3 : 0xf3edf8),
        0);
    lv_obj_set_style_text_color(
        s_follow_label,
        lv_color_hex(s_auto_follow ? 0x4e9d84 : 0x8a78a0),
        0);
}

static void chat_scroll_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (s_programmatic_scroll) return;
    if (code == LV_EVENT_SCROLL_BEGIN) {
        s_auto_follow = false;
    } else if (code == LV_EVENT_SCROLL_END) {
        s_auto_follow = lv_obj_get_scroll_bottom(s_chat_scroll) <= 12;
    } else {
        return;
    }
    follow_button_refresh();
}

static void follow_button_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    s_auto_follow = !s_auto_follow;
    follow_button_refresh();
    if (s_auto_follow) scroll_chat_to_bottom(LV_ANIM_OFF);
}

static void reset_button_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    agent_app_reset_session();
    s_auto_follow = true;
    s_reset_feedback_until_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000) + 1200U;
    follow_button_refresh();
    snprintf(s_reply_text, sizeof(s_reply_text),
             "对话已重置。\n\nAgent 会在下一次请求时建立新的上下文。");
    lv_label_set_text(s_reply_label, s_reply_text);
    scroll_chat_to_bottom(LV_ANIM_OFF);
}

static void status_pill_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    agent_app_phase_t phase = agent_app_phase();
    if (agent_phase_is_busy(phase)) {
        agent_app_cancel();
    } else if (phase == AGENT_APP_PHASE_COMPLETED || phase == AGENT_APP_PHASE_CANCELLED ||
               phase == AGENT_APP_PHASE_ERROR) {
        reset_button_event_cb(event);
    }
}

static void record_button_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || s_audio_command_sem == nullptr) return;
    ui_audio_state_t state = s_audio_state.load();
    if (state == UI_AUDIO_READY) {
        s_record_requested.store(true);
        s_audio_state.store(UI_AUDIO_STARTING);
        xSemaphoreGive(s_audio_command_sem);
    } else if (state == UI_AUDIO_STARTING || state == UI_AUDIO_RECORDING) {
        s_record_requested.store(false);
        s_audio_state.store(UI_AUDIO_STOPPING);
        xSemaphoreGive(s_audio_command_sem);
    } else if (state == UI_AUDIO_UNAVAILABLE || state == UI_AUDIO_ERROR) {
        s_audio_retry_requested.store(true);
        s_audio_state.store(UI_AUDIO_INITIALIZING);
        xSemaphoreGive(s_audio_command_sem);
    }
}

static bool fonts_and_characters_prepare(void)
{
    s_font_24 = lv_binfont_create_from_buffer((void *)_binary_noto_zh_mid_24_bin_start,
                                              (uint32_t)(_binary_noto_zh_mid_24_bin_end -
                                                         _binary_noto_zh_mid_24_bin_start));
    if (s_font_24 == nullptr) {
        ESP_LOGE(TAG, "noto_zh_mid_24.bin load failed");
    } else {
        /* The bundled CJK font owns Chinese glyphs; missing Latin/symbol glyphs
         * fall back to LVGL's built-in font at the same 24 px size. */
        s_font_24->fallback = &lv_font_montserrat_24;
        ESP_LOGI(TAG, "24 px CJK font loaded with Montserrat 24 fallback");
    }

    bool ready = true;
    for (int i = 0; i < UI_CHARACTER_COUNT; ++i) {
        size_t embedded_size = (size_t)(CHARACTER_ASSETS[i].end - CHARACTER_ASSETS[i].start);
        if (embedded_size != CHARACTER_BYTES) {
            ESP_LOGE(TAG, "character %s size mismatch: expected=%u actual=%u",
                     CHARACTER_ASSETS[i].name, (unsigned)CHARACTER_BYTES,
                     (unsigned)embedded_size);
            ready = false;
            continue;
        }

        lv_image_dsc_t &dsc = s_character_dsc[i];
        dsc = {};
        dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
        dsc.header.flags = 0;
        dsc.header.w = CHARACTER_W;
        dsc.header.h = CHARACTER_H;
        dsc.header.stride = CHARACTER_STRIDE;
        dsc.data_size = CHARACTER_BYTES;
        dsc.data = CHARACTER_ASSETS[i].start;
    }
    return ready;
}

static ui_character_state_t character_for_phase(agent_app_phase_t phase)
{
    switch (phase) {
    case AGENT_APP_PHASE_LISTENING: return UI_CHARACTER_LISTENING;
    case AGENT_APP_PHASE_THINKING: return UI_CHARACTER_THINKING;
    case AGENT_APP_PHASE_TOOL: return UI_CHARACTER_TOOL;
    case AGENT_APP_PHASE_RESPONDING: return UI_CHARACTER_SPEAKING;
    case AGENT_APP_PHASE_COMPLETED: return UI_CHARACTER_SUCCESS;
    case AGENT_APP_PHASE_DISABLED:
    case AGENT_APP_PHASE_ERROR: return UI_CHARACTER_ERROR;
    default: return UI_CHARACTER_IDLE;
    }
}

static const char *phase_title(agent_app_phase_t phase)
{
    switch (phase) {
    case AGENT_APP_PHASE_BOOTING: return "正在启动";
    case AGENT_APP_PHASE_CONNECTING: return "连接网络";
    case AGENT_APP_PHASE_READY: return "准备就绪";
    case AGENT_APP_PHASE_LISTENING: return "正在聆听";
    case AGENT_APP_PHASE_THINKING: return "正在思考";
    case AGENT_APP_PHASE_TOOL: return "调用工具";
    case AGENT_APP_PHASE_RESPONDING: return "生成回复";
    case AGENT_APP_PHASE_COMPLETED: return "任务完成";
    case AGENT_APP_PHASE_CANCELLED: return "已停止";
    case AGENT_APP_PHASE_DISABLED: return "等待配置";
    case AGENT_APP_PHASE_ERROR: return "暂时离线";
    default: return "状态未知";
    }
}

static const char *phase_caption(agent_app_phase_t phase)
{
    switch (phase) {
    case AGENT_APP_PHASE_BOOTING: return "正在初始化设备服务";
    case AGENT_APP_PHASE_CONNECTING: return "正在等待网络连接";
    case AGENT_APP_PHASE_READY: return "我已经准备好了";
    case AGENT_APP_PHASE_LISTENING: return "请继续，我在听";
    case AGENT_APP_PHASE_THINKING: return "让我认真想一想...";
    case AGENT_APP_PHASE_TOOL: return "正在读取设备信息";
    case AGENT_APP_PHASE_RESPONDING: return "正在为你组织回答";
    case AGENT_APP_PHASE_COMPLETED: return "这次任务顺利完成";
    case AGENT_APP_PHASE_CANCELLED: return "当前任务已经停止";
    case AGENT_APP_PHASE_DISABLED: return "请先在 sdkconfig 配置密钥";
    case AGENT_APP_PHASE_ERROR: return "连接异常，稍后再试";
    default: return "";
    }
}

static uint32_t phase_color(agent_app_phase_t phase)
{
    switch (phase) {
    case AGENT_APP_PHASE_THINKING: return 0xff8bb7;
    case AGENT_APP_PHASE_TOOL: return 0x8e9cff;
    case AGENT_APP_PHASE_RESPONDING: return 0x65c9d0;
    case AGENT_APP_PHASE_COMPLETED: return 0x70c9a5;
    case AGENT_APP_PHASE_DISABLED:
    case AGENT_APP_PHASE_ERROR: return 0xf09a75;
    case AGENT_APP_PHASE_CONNECTING: return 0xf2bf67;
    default: return 0x79cbb0;
    }
}

static void update_phase_visual(agent_app_phase_t phase, bool capture_active)
{
    if (phase == s_last_phase && capture_active == s_last_capture_visual) {
        return;
    }
    s_last_phase = phase;
    s_last_capture_visual = capture_active;

    agent_app_phase_t character_phase = capture_active ? AGENT_APP_PHASE_LISTENING : phase;

    snprintf(s_status_text, sizeof(s_status_text), "%s", phase_title(phase));
    lv_label_set_text(s_status_label, s_status_text);
    lv_obj_set_style_bg_color(s_status_dot, lv_color_hex(phase_color(phase)), 0);
    lv_label_set_text(s_character_state_label, phase_title(character_phase));
    lv_label_set_text(s_character_caption, phase_caption(character_phase));

    if (s_character_image != nullptr) {
        lv_image_set_src(s_character_image, &s_character_dsc[character_for_phase(character_phase)]);
    }

    bool thinking = character_phase == AGENT_APP_PHASE_THINKING;
    if (thinking) {
        lv_obj_remove_flag(s_thinking_spinner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_thinking_hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_thinking_spinner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_thinking_hint, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_status_action_label != nullptr && s_status_pill != nullptr) {
        if (agent_phase_is_busy(phase)) {
            lv_label_set_text(s_status_action_label, "STOP");
            lv_obj_add_flag(s_status_pill, LV_OBJ_FLAG_CLICKABLE);
        } else if (phase == AGENT_APP_PHASE_COMPLETED || phase == AGENT_APP_PHASE_CANCELLED ||
                   phase == AGENT_APP_PHASE_ERROR) {
            lv_label_set_text(s_status_action_label, "RESET");
            lv_obj_add_flag(s_status_pill, LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_label_set_text(s_status_action_label, "");
            lv_obj_remove_flag(s_status_pill, LV_OBJ_FLAG_CLICKABLE);
        }
    }
}

static void update_metrics(void)
{
    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    if (uptime_s == s_last_metric_second) {
        return;
    }
    s_last_metric_second = uptime_s;

    size_t heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    lv_label_set_text_fmt(s_heap_label, "HEAP\n%u KB", (unsigned)(heap_free / 1024));
    if (psram_free >= 1024 * 1024) {
        lv_label_set_text_fmt(s_psram_label, "PSRAM\n%u.%u MB",
                              (unsigned)(psram_free / (1024 * 1024)),
                              (unsigned)((psram_free % (1024 * 1024)) * 10 / (1024 * 1024)));
    } else {
        lv_label_set_text_fmt(s_psram_label, "PSRAM\n%u KB", (unsigned)(psram_free / 1024));
    }
    lv_label_set_text_fmt(s_uptime_label, "UPTIME\n%02u:%02u:%02u",
                          (unsigned)(uptime_s / 3600), (unsigned)((uptime_s / 60) % 60),
                          (unsigned)(uptime_s % 60));
}

static void update_audio_visual(void)
{
    if (s_record_button == nullptr || s_transcript_text == nullptr) return;

    ui_audio_state_t state = s_audio_state.load();
    uint32_t level = s_audio_level_percent.load();
    uint32_t bytes = s_recorded_bytes.load();
    uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    char transcript[CONFIG_OTOOL_SPEECH_ASR_MAX_TRANSCRIPT_BYTES] = "";
    char notice[sizeof(s_speech_notice)] = "";
    speech_text_copy(transcript, sizeof(transcript), notice, sizeof(notice));

    if (state != s_last_audio_state) {
        s_last_audio_state = state;
        lv_obj_remove_state(s_record_button, LV_STATE_DISABLED);
        switch (state) {
        case UI_AUDIO_INITIALIZING:
            lv_label_set_text(s_record_icon, LV_SYMBOL_REFRESH);
            lv_label_set_text(s_record_state_label, "INIT");
            lv_label_set_text(s_transcript_title, "LIVE CAPTURE | INITIALIZING");
            lv_obj_set_style_bg_color(s_record_button, lv_color_hex(0x8e8aa4), 0);
            lv_obj_add_state(s_record_button, LV_STATE_DISABLED);
            break;
        case UI_AUDIO_READY:
            lv_label_set_text(s_record_icon, LV_SYMBOL_AUDIO);
            lv_label_set_text(s_record_state_label, "READY");
            lv_label_set_text(s_transcript_title, "VOICE AGENT | 16K | 16BIT | MONO ASR");
            lv_obj_set_style_bg_color(s_record_button, lv_color_hex(0xff7da9), 0);
            break;
        case UI_AUDIO_STARTING:
            lv_label_set_text(s_record_icon, LV_SYMBOL_AUDIO);
            lv_label_set_text(s_record_state_label, "OPEN");
            lv_label_set_text(s_transcript_title, "LIVE CAPTURE | STARTING");
            lv_obj_set_style_bg_color(s_record_button, lv_color_hex(0xf1a063), 0);
            lv_obj_add_state(s_record_button, LV_STATE_DISABLED);
            break;
        case UI_AUDIO_RECORDING:
            lv_label_set_text(s_record_icon, LV_SYMBOL_STOP);
            lv_label_set_text(s_record_state_label, "REC");
            lv_label_set_text(s_transcript_title, "VOICE AGENT | STREAMING ASR");
            lv_obj_set_style_bg_color(s_record_button, lv_color_hex(0xf05f7f), 0);
            break;
        case UI_AUDIO_STOPPING:
            lv_label_set_text(s_record_icon, LV_SYMBOL_STOP);
            lv_label_set_text(s_record_state_label, "WAIT");
            lv_label_set_text(s_transcript_title, "LIVE CAPTURE | FINALIZING");
            lv_obj_set_style_bg_color(s_record_button, lv_color_hex(0xa98ca7), 0);
            lv_obj_add_state(s_record_button, LV_STATE_DISABLED);
            break;
        case UI_AUDIO_TRANSCRIBING:
            lv_label_set_text(s_record_icon, LV_SYMBOL_REFRESH);
            lv_label_set_text(s_record_state_label, "ASR");
            lv_label_set_text(s_transcript_title, "VOICE AGENT | FINAL TRANSCRIPT");
            lv_obj_set_style_bg_color(s_record_button, lv_color_hex(0x938bd0), 0);
            lv_obj_add_state(s_record_button, LV_STATE_DISABLED);
            break;
        case UI_AUDIO_WAITING_AGENT:
            lv_label_set_text(s_record_icon, LV_SYMBOL_REFRESH);
            lv_label_set_text(s_record_state_label, "THINK");
            lv_label_set_text(s_transcript_title, "VOICE AGENT | AGENT WORKING");
            lv_obj_set_style_bg_color(s_record_button, lv_color_hex(0x8e9cff), 0);
            lv_obj_add_state(s_record_button, LV_STATE_DISABLED);
            break;
        case UI_AUDIO_SYNTHESIZING:
            lv_label_set_text(s_record_icon, LV_SYMBOL_REFRESH);
            lv_label_set_text(s_record_state_label, "TTS");
            lv_label_set_text(s_transcript_title, "VOICE AGENT | SYNTHESIZING");
            lv_obj_set_style_bg_color(s_record_button, lv_color_hex(0x65c9d0), 0);
            lv_obj_add_state(s_record_button, LV_STATE_DISABLED);
            break;
        case UI_AUDIO_PLAYING:
            lv_label_set_text(s_record_icon, LV_SYMBOL_VOLUME_MAX);
            lv_label_set_text(s_record_state_label, "PLAY");
            lv_label_set_text(s_transcript_title, "VOICE AGENT | PLAYING RESPONSE");
            lv_obj_set_style_bg_color(s_record_button, lv_color_hex(0x70c9a5), 0);
            lv_obj_add_state(s_record_button, LV_STATE_DISABLED);
            break;
        case UI_AUDIO_SPEECH_DISABLED:
            lv_label_set_text(s_record_icon, LV_SYMBOL_CLOSE);
            lv_label_set_text(s_record_state_label, "CONFIG");
            lv_label_set_text(s_transcript_title, "VOICE AGENT | SERVICE DISABLED");
            lv_obj_set_style_bg_color(s_record_button, lv_color_hex(0x9c96aa), 0);
            lv_obj_add_state(s_record_button, LV_STATE_DISABLED);
            break;
        case UI_AUDIO_UNAVAILABLE:
            lv_label_set_text(s_record_icon, LV_SYMBOL_REFRESH);
            lv_label_set_text(s_record_state_label, "RETRY");
            lv_label_set_text(s_transcript_title, "LIVE CAPTURE | AUDIO OFFLINE");
            lv_obj_set_style_bg_color(s_record_button, lv_color_hex(0xc78077), 0);
            break;
        case UI_AUDIO_ERROR:
            lv_label_set_text(s_record_icon, LV_SYMBOL_REFRESH);
            lv_label_set_text(s_record_state_label, "RETRY");
            lv_label_set_text(s_transcript_title, "VOICE AGENT | ERROR");
            lv_obj_set_style_bg_color(s_record_button, lv_color_hex(0xd66472), 0);
            break;
        }
    }

    switch (state) {
    case UI_AUDIO_INITIALIZING:
        lv_label_set_text(s_transcript_text, "正在初始化 ES7210 与 I2S 录音链路...");
        break;
    case UI_AUDIO_READY:
        if (transcript[0] != '\0') {
            lv_label_set_text_fmt(s_transcript_text, "%s\n%s", transcript,
                                  notice[0] != '\0' ? notice : "轻触左侧按钮开始下一轮");
        } else if (notice[0] != '\0') {
            lv_label_set_text(s_transcript_text, notice);
        } else if (bytes == 0) {
            lv_label_set_text(s_transcript_text, "轻触左侧按钮开始语音对话");
        } else {
            uint32_t duration_tenths = static_cast<uint32_t>(
                static_cast<uint64_t>(bytes) * 10U /
                (AUDIO_SAMPLE_RATE_HZ * m5::tab5::M5TAB5_AUDIO_RECORD_CHANNELS *
                 sizeof(int16_t)));
            lv_label_set_text_fmt(s_transcript_text,
                                  "录音已结束 | %u.%u 秒 | %u KB PCM 已采集",
                                  (unsigned)(duration_tenths / 10),
                                  (unsigned)(duration_tenths % 10),
                                  (unsigned)(bytes / 1024));
        }
        break;
    case UI_AUDIO_STARTING:
        lv_label_set_text(s_transcript_text,
                          notice[0] != '\0' ? notice : "正在打开麦克风阵列...");
        break;
    case UI_AUDIO_RECORDING: {
        uint32_t elapsed_ms = now_ms - s_capture_started_ms.load();
        uint32_t elapsed_s = elapsed_ms / 1000;
        if (transcript[0] != '\0') {
            lv_label_set_text_fmt(s_transcript_text,
                                  "%s\n录音中 %02u:%02u | 电平 %u%% | %u KB",
                                  transcript, (unsigned)(elapsed_s / 60),
                                  (unsigned)(elapsed_s % 60), (unsigned)level,
                                  (unsigned)(bytes / 1024));
        } else {
            lv_label_set_text_fmt(s_transcript_text,
                                  "录音中 %02u:%02u | 电平 %u%% | %u KB",
                                  (unsigned)(elapsed_s / 60),
                                  (unsigned)(elapsed_s % 60), (unsigned)level,
                                  (unsigned)(bytes / 1024));
        }
        break;
    }
    case UI_AUDIO_STOPPING:
        lv_label_set_text(s_transcript_text, "正在完成本次采集...");
        break;
    case UI_AUDIO_TRANSCRIBING:
    case UI_AUDIO_WAITING_AGENT:
    case UI_AUDIO_SYNTHESIZING:
    case UI_AUDIO_PLAYING:
        if (transcript[0] != '\0') {
            lv_label_set_text_fmt(s_transcript_text, "%s\n%s", transcript,
                                  notice[0] != '\0' ? notice : "处理中...");
        } else {
            lv_label_set_text(s_transcript_text,
                              notice[0] != '\0' ? notice : "处理中...");
        }
        break;
    case UI_AUDIO_SPEECH_DISABLED:
        lv_label_set_text(s_transcript_text,
                          "语音服务未启用：请在 menuconfig 的 otool_tab5_live2d app 中"
                          "配置 Speech API Key 后重新编译");
        break;
    case UI_AUDIO_UNAVAILABLE:
    case UI_AUDIO_ERROR:
        if (notice[0] != '\0') {
            lv_label_set_text_fmt(s_transcript_text, "%s | %s", notice,
                                  esp_err_to_name(s_audio_error.load()));
        } else {
            lv_label_set_text_fmt(s_transcript_text,
                                  "语音链路不可用：%s | 轻触左侧按钮重试",
                                  esp_err_to_name(s_audio_error.load()));
        }
        break;
    }

    static const uint8_t wave_scale[5] = {52, 78, 100, 66, 88};
    for (int i = 0; i < 5; ++i) {
        uint32_t animated_level = state == UI_AUDIO_RECORDING ? level : 0;
        if (state == UI_AUDIO_RECORDING) {
            uint32_t pulse = (now_ms / 90U + static_cast<uint32_t>(i) * 2U) % 5U;
            animated_level = (animated_level * (80U + pulse * 5U)) / 100U;
        }
        int32_t height = 8 + static_cast<int32_t>(animated_level * wave_scale[i] / 210U);
        if (height > 52) height = 52;
        lv_obj_set_y(s_wave_bars[i], 50 - height / 2);
        lv_obj_set_height(s_wave_bars[i], height);
        lv_obj_set_style_bg_color(
            s_wave_bars[i],
            lv_color_hex(state == UI_AUDIO_RECORDING
                             ? (i % 2 == 0 ? 0xf05f7f : 0x918fe8)
                             : 0xc9bfd2),
            0);
    }
}

static void ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_status_label == nullptr || s_reply_label == nullptr) {
        return;
    }

    agent_app_phase_t phase = agent_app_phase();
    ui_audio_state_t audio_state = s_audio_state.load();
    agent_app_phase_t visual_phase = phase;
    if (audio_state == UI_AUDIO_SYNTHESIZING || audio_state == UI_AUDIO_PLAYING) {
        visual_phase = AGENT_APP_PHASE_RESPONDING;
    }
    update_audio_visual();
    update_phase_visual(visual_phase, capture_is_active(audio_state));
    update_metrics();

    uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    bool reset_feedback =
        static_cast<int32_t>(s_reset_feedback_until_ms - now_ms) > 0;
    size_t reply_len = reset_feedback
        ? strlen(s_reply_text)
        : agent_app_reply_read(s_reply_text, sizeof(s_reply_text));
    if (reply_len == 0) {
        switch (phase) {
        case AGENT_APP_PHASE_DISABLED:
            snprintf(s_reply_text, sizeof(s_reply_text),
                     "Agent 当前处于安全禁用状态。\n\n请在 sdkconfig 中配置 API Key 并重新编译；"
                     "没有密钥时界面仍可正常运行，不会发起模型请求。");
            break;
        case AGENT_APP_PHASE_CONNECTING:
            snprintf(s_reply_text, sizeof(s_reply_text),
                     "正在等待设备网络连接。\n\n连接恢复后 Agent 会自动进入就绪状态。");
            break;
        case AGENT_APP_PHASE_THINKING:
            snprintf(s_reply_text, sizeof(s_reply_text),
                     "正在分析你的请求，并规划下一步操作...");
            break;
        case AGENT_APP_PHASE_TOOL:
            snprintf(s_reply_text, sizeof(s_reply_text),
                     "正在调用设备工具并核对返回结果...");
            break;
        case AGENT_APP_PHASE_ERROR:
            snprintf(s_reply_text, sizeof(s_reply_text),
                     "Agent 链路暂时异常。\n\n界面和设备遥测保持可用，请稍后重新发起任务。");
            break;
        default:
            snprintf(s_reply_text, sizeof(s_reply_text),
                     "Agent 工作区已就绪。\n\n轻触左下角录音按钮即可开始实时识别；"
                     "识别结果会自动交给 Agent，并在配置音色后播放回复。");
            break;
        }
    }

    if (strcmp(lv_label_get_text(s_reply_label), s_reply_text) != 0) {
        lv_label_set_text(s_reply_label, s_reply_text);
        lv_obj_update_layout(s_chat_scroll);
        if (s_auto_follow) scroll_chat_to_bottom(LV_ANIM_OFF);
    }
}

extern "C" void ui_app_start(void *tab5_comp)
{
    s_tab5 = static_cast<m5::tab5::otool_tab5_component *>(tab5_comp);
    if (s_audio_command_sem == nullptr) {
        s_audio_command_sem = xSemaphoreCreateBinary();
    }
    if (s_speech_text_lock == nullptr) {
        s_speech_text_lock = xSemaphoreCreateMutex();
    }
    if (s_audio_command_sem == nullptr || s_speech_text_lock == nullptr) {
        s_audio_error.store(ESP_ERR_NO_MEM);
        s_audio_state.store(UI_AUDIO_UNAVAILABLE);
    }

    bool characters_ready = fonts_and_characters_prepare();

    otool_lvgl_port_lock(0);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_size(screen, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xf8f6ff), 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_hex(0xe9eaff), 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_CLICKABLE);

    /* Anime-inspired atmosphere, kept behind a professional information grid. */
    decor_circle(screen, -96, -104, 260, 0xffbfdc, LV_OPA_40);
    decor_circle(screen, 720, 588, 230, 0xc9c8ff, LV_OPA_30);
    decor_circle(screen, 1175, -66, 170, 0xffe3a5, LV_OPA_40);

    /* Left two-thirds: telemetry, streaming conversation, and audio composer. */
    lv_obj_t *left_panel = lv_obj_create(screen);
    lv_obj_set_pos(left_panel, LEFT_X, PANEL_Y);
    lv_obj_set_size(left_panel, LEFT_W, PANEL_H);
    style_card(left_panel, lv_color_hex(0xfffcff), 28);
    lv_obj_set_style_border_width(left_panel, 1, 0);
    lv_obj_set_style_border_color(left_panel, lv_color_hex(0xe5deef), 0);
    lv_obj_set_style_shadow_width(left_panel, 22, 0);
    lv_obj_set_style_shadow_offset_y(left_panel, 7, 0);
    lv_obj_set_style_shadow_color(left_panel, lv_color_hex(0x796d98), 0);
    lv_obj_set_style_shadow_opa(left_panel, LV_OPA_20, 0);
    make_non_interactive(left_panel);

    /* Performance monitor: intentionally pinned to the upper-left. */
    lv_obj_t *telemetry = lv_obj_create(left_panel);
    lv_obj_set_pos(telemetry, 20, 18);
    lv_obj_set_size(telemetry, 326, 82);
    style_card(telemetry, lv_color_hex(0x29283d), 18);
    lv_obj_set_style_bg_grad_color(telemetry, lv_color_hex(0x47415f), 0);
    lv_obj_set_style_bg_grad_dir(telemetry, LV_GRAD_DIR_HOR, 0);
    make_non_interactive(telemetry);

    lv_obj_t *telemetry_title = lv_label_create(telemetry);
    lv_label_set_text(telemetry_title, "SYSTEM PERFORMANCE");
    lv_obj_set_pos(telemetry_title, 14, 9);
    lv_obj_set_style_text_font(telemetry_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(telemetry_title, lv_color_hex(0xffb8d5), 0);
    lv_obj_set_style_text_letter_space(telemetry_title, 1, 0);

    s_heap_label = metric_label_create(telemetry, 14, "HEAP\n-- KB");
    s_psram_label = metric_label_create(telemetry, 112, "PSRAM\n-- MB");
    s_uptime_label = metric_label_create(telemetry, 216, "UPTIME\n00:00:00");

    lv_obj_t *workspace_title = lv_label_create(left_panel);
    lv_label_set_text(workspace_title, "OTOOL // AGENT");
    lv_obj_set_pos(workspace_title, 370, 18);
    lv_obj_set_style_text_font(workspace_title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(workspace_title, lv_color_hex(0x3f3953), 0);

    lv_obj_t *workspace_subtitle = lv_label_create(left_panel);
    lv_label_set_text(workspace_subtitle, "设备交互中枢");
    lv_obj_set_pos(workspace_subtitle, 370, 53);
    if (s_font_24 != nullptr) {
        lv_obj_set_style_text_font(workspace_subtitle, s_font_24, 0);
    }
    lv_obj_set_style_text_color(workspace_subtitle, lv_color_hex(0x988ba8), 0);

    s_status_pill = lv_obj_create(left_panel);
    lv_obj_set_pos(s_status_pill, 584, 25);
    lv_obj_set_size(s_status_pill, 216, 54);
    style_card(s_status_pill, lv_color_hex(0xf1edf7), 27);
    lv_obj_remove_flag(s_status_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_translate_y(s_status_pill, 2, LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_status_pill, status_pill_event_cb, LV_EVENT_CLICKED, nullptr);

    s_status_dot = decor_circle(s_status_pill, 16, 19, 16, 0x79cbb0, LV_OPA_COVER);
    s_status_label = lv_label_create(s_status_pill);
    lv_label_set_text(s_status_label, "正在启动");
    lv_obj_set_pos(s_status_label, 43, 13);
    lv_obj_set_width(s_status_label, 110);
    if (s_font_24 != nullptr) {
        lv_obj_set_style_text_font(s_status_label, s_font_24, 0);
    }
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x5d566c), 0);

    s_status_action_label = lv_label_create(s_status_pill);
    lv_label_set_text(s_status_action_label, "");
    lv_obj_align(s_status_action_label, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_set_style_text_font(s_status_action_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_status_action_label, lv_color_hex(0xa06c88), 0);

    lv_obj_t *chat_card = lv_obj_create(left_panel);
    lv_obj_set_pos(chat_card, 20, 116);
    lv_obj_set_size(chat_card, 780, 426);
    style_card(chat_card, lv_color_hex(0xf9f7fc), 22);
    lv_obj_set_style_border_width(chat_card, 1, 0);
    lv_obj_set_style_border_color(chat_card, lv_color_hex(0xe7dfed), 0);
    make_non_interactive(chat_card);

    lv_obj_t *rail = lv_obj_create(chat_card);
    lv_obj_set_pos(rail, 0, 0);
    lv_obj_set_size(rail, 8, 426);
    style_card(rail, lv_color_hex(0xff8eb9), 8);
    lv_obj_set_style_bg_grad_color(rail, lv_color_hex(0x8f91ee), 0);
    lv_obj_set_style_bg_grad_dir(rail, LV_GRAD_DIR_VER, 0);
    make_non_interactive(rail);

    lv_obj_t *ai_badge = lv_obj_create(chat_card);
    lv_obj_set_pos(ai_badge, 24, 18);
    lv_obj_set_size(ai_badge, 58, 38);
    style_card(ai_badge, lv_color_hex(0xff88b5), 19);
    make_non_interactive(ai_badge);

    lv_obj_t *ai_badge_label = lv_label_create(ai_badge);
    lv_label_set_text(ai_badge_label, "AI");
    lv_obj_center(ai_badge_label);
    lv_obj_set_style_text_font(ai_badge_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(ai_badge_label, lv_color_white(), 0);

    lv_obj_t *conversation_title = lv_label_create(chat_card);
    lv_label_set_text(conversation_title, "AGENT STREAM");
    lv_obj_set_pos(conversation_title, 98, 18);
    lv_obj_set_style_text_font(conversation_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(conversation_title, 1, 0);
    lv_obj_set_style_text_color(conversation_title, lv_color_hex(0x766a84), 0);

    lv_obj_t *conversation_subtitle = lv_label_create(chat_card);
    lv_label_set_text(conversation_subtitle, "回复与工具执行结果会实时汇入此处");
    lv_obj_set_pos(conversation_subtitle, 98, 37);
    if (s_font_24 != nullptr) {
        lv_obj_set_style_text_font(conversation_subtitle, s_font_24, 0);
    }
    lv_obj_set_style_text_color(conversation_subtitle, lv_color_hex(0x9b8da4), 0);

    lv_obj_t *reset_button = lv_obj_create(chat_card);
    lv_obj_set_pos(reset_button, 536, 18);
    lv_obj_set_size(reset_button, 104, 36);
    style_card(reset_button, lv_color_hex(0xf3edf8), 18);
    lv_obj_remove_flag(reset_button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_translate_y(reset_button, 2, LV_STATE_PRESSED);
    lv_obj_add_event_cb(reset_button, reset_button_event_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *reset_label = lv_label_create(reset_button);
    lv_label_set_text(reset_label, "RESET");
    lv_obj_center(reset_label);
    lv_obj_set_style_text_font(reset_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(reset_label, lv_color_hex(0x8a78a0), 0);

    s_follow_button = lv_obj_create(chat_card);
    lv_obj_set_pos(s_follow_button, 650, 18);
    lv_obj_set_size(s_follow_button, 104, 36);
    style_card(s_follow_button, lv_color_hex(0xe9f8f3), 18);
    lv_obj_remove_flag(s_follow_button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_translate_y(s_follow_button, 2, LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_follow_button, follow_button_event_cb, LV_EVENT_CLICKED, nullptr);
    s_follow_label = lv_label_create(s_follow_button);
    lv_label_set_text(s_follow_label, "FOLLOW");
    lv_obj_center(s_follow_label);
    lv_obj_set_style_text_font(s_follow_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_follow_label, lv_color_hex(0x4e9d84), 0);

    lv_obj_t *chat_divider = lv_obj_create(chat_card);
    lv_obj_set_pos(chat_divider, 24, 72);
    lv_obj_set_size(chat_divider, 730, 1);
    style_card(chat_divider, lv_color_hex(0xe9e2ee), 1);
    make_non_interactive(chat_divider);

    s_chat_scroll = lv_obj_create(chat_card);
    lv_obj_set_pos(s_chat_scroll, 24, 88);
    lv_obj_set_size(s_chat_scroll, 730, 316);
    style_card(s_chat_scroll, lv_color_hex(0xffffff), 18);
    lv_obj_set_style_border_width(s_chat_scroll, 1, 0);
    lv_obj_set_style_border_color(s_chat_scroll, lv_color_hex(0xeee7f2), 0);
    lv_obj_set_style_pad_all(s_chat_scroll, 0, 0);
    lv_obj_set_scroll_dir(s_chat_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_chat_scroll, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(s_chat_scroll, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_chat_scroll, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_add_event_cb(s_chat_scroll, chat_scroll_event_cb, LV_EVENT_SCROLL_BEGIN, nullptr);
    lv_obj_add_event_cb(s_chat_scroll, chat_scroll_event_cb, LV_EVENT_SCROLL_END, nullptr);

    s_reply_label = lv_label_create(s_chat_scroll);
    lv_label_set_text(s_reply_label, "");
    lv_label_set_long_mode(s_reply_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_reply_label, 680);
    lv_obj_set_height(s_reply_label, LV_SIZE_CONTENT);
    lv_obj_set_pos(s_reply_label, 22, 18);
    if (s_font_24 != nullptr) {
        lv_obj_set_style_text_font(s_reply_label, s_font_24, 0);
    }
    lv_obj_set_style_text_color(s_reply_label, lv_color_hex(0x443d4b), 0);
    lv_obj_set_style_text_line_space(s_reply_label, 10, 0);

    /* Audio composer: interactive ES7210 capture with live level feedback. */
    lv_obj_t *composer = lv_obj_create(left_panel);
    lv_obj_set_pos(composer, 20, 558);
    lv_obj_set_size(composer, 780, 106);
    style_card(composer, lv_color_hex(0x302e44), 22);
    lv_obj_set_style_bg_grad_color(composer, lv_color_hex(0x44405d), 0);
    lv_obj_set_style_bg_grad_dir(composer, LV_GRAD_DIR_HOR, 0);
    make_non_interactive(composer);

    s_record_button = lv_obj_create(composer);
    lv_obj_set_pos(s_record_button, 14, 13);
    lv_obj_set_size(s_record_button, 80, 80);
    style_card(s_record_button, lv_color_hex(0xff7da9), LV_RADIUS_CIRCLE);
    lv_obj_set_style_shadow_width(s_record_button, 12, 0);
    lv_obj_set_style_shadow_color(s_record_button, lv_color_hex(0xff7da9), 0);
    lv_obj_set_style_shadow_opa(s_record_button, LV_OPA_30, 0);
    lv_obj_remove_flag(s_record_button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_transform_scale(s_record_button, 244, LV_STATE_PRESSED);
    lv_obj_set_style_opa(s_record_button, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_record_button, record_button_event_cb, LV_EVENT_CLICKED, nullptr);

    s_record_icon = lv_label_create(s_record_button);
    lv_label_set_text(s_record_icon, LV_SYMBOL_REFRESH);
    lv_obj_align(s_record_icon, LV_ALIGN_CENTER, 0, -5);
    lv_obj_set_style_text_font(s_record_icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_record_icon, lv_color_white(), 0);

    s_record_state_label = lv_label_create(s_record_button);
    lv_label_set_text(s_record_state_label, "INIT");
    lv_obj_align(s_record_state_label, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_text_font(s_record_state_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_record_state_label, lv_color_hex(0xffeff6), 0);

    lv_obj_t *transcript = lv_obj_create(composer);
    lv_obj_set_pos(transcript, 110, 13);
    lv_obj_set_size(transcript, 654, 80);
    style_card(transcript, lv_color_hex(0xf9f7ff), 16);
    lv_obj_set_style_bg_opa(transcript, LV_OPA_90, 0);
    make_non_interactive(transcript);

    s_transcript_title = lv_label_create(transcript);
    lv_label_set_text(s_transcript_title, "LIVE CAPTURE | INITIALIZING");
    lv_obj_set_pos(s_transcript_title, 18, 10);
    lv_obj_set_style_text_font(s_transcript_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_transcript_title, lv_color_hex(0x8a78a0), 0);

    s_transcript_text = lv_label_create(transcript);
    lv_label_set_text(s_transcript_text, "正在初始化录音链路...");
    lv_obj_set_pos(s_transcript_text, 18, 36);
    lv_obj_set_width(s_transcript_text, 540);
    if (s_font_24 != nullptr) {
        lv_obj_set_style_text_font(s_transcript_text, s_font_24, 0);
    }
    lv_obj_set_style_text_color(s_transcript_text, lv_color_hex(0x50475c), 0);

    const int32_t wave_heights[] = {18, 34, 48, 26, 40};
    for (int i = 0; i < 5; ++i) {
        s_wave_bars[i] = lv_obj_create(transcript);
        lv_obj_set_pos(s_wave_bars[i], 574 + i * 11, 40 - wave_heights[i] / 2);
        lv_obj_set_size(s_wave_bars[i], 6, wave_heights[i]);
        style_card(s_wave_bars[i], lv_color_hex(i % 2 == 0 ? 0xff86b2 : 0x918fe8), 3);
        make_non_interactive(s_wave_bars[i]);
    }

    /* Right one-third: stateful character stage. */
    lv_obj_t *right_panel = lv_obj_create(screen);
    lv_obj_set_pos(right_panel, RIGHT_X, PANEL_Y);
    lv_obj_set_size(right_panel, RIGHT_W, PANEL_H);
    style_card(right_panel, lv_color_hex(0xffd9e8), 28);
    lv_obj_set_style_bg_grad_color(right_panel, lv_color_hex(0xdedfff), 0);
    lv_obj_set_style_bg_grad_dir(right_panel, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(right_panel, 1, 0);
    lv_obj_set_style_border_color(right_panel, lv_color_hex(0xe0cfe5), 0);
    lv_obj_set_style_shadow_width(right_panel, 22, 0);
    lv_obj_set_style_shadow_offset_y(right_panel, 7, 0);
    lv_obj_set_style_shadow_color(right_panel, lv_color_hex(0x74658f), 0);
    lv_obj_set_style_shadow_opa(right_panel, LV_OPA_20, 0);
    make_non_interactive(right_panel);

    decor_circle(right_panel, 285, 20, 92, 0xffe9a7, LV_OPA_60);
    decor_circle(right_panel, 8, 268, 72, 0xffffff, LV_OPA_40);

    lv_obj_t *avatar_kicker = lv_label_create(right_panel);
    lv_label_set_text(avatar_kicker, "AGENT // AVATAR");
    lv_obj_set_pos(avatar_kicker, 22, 18);
    lv_obj_set_style_text_font(avatar_kicker, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(avatar_kicker, 1, 0);
    lv_obj_set_style_text_color(avatar_kicker, lv_color_hex(0x7d6b91), 0);

    s_character_state_label = lv_label_create(right_panel);
    lv_label_set_text(s_character_state_label, "正在启动");
    lv_obj_set_pos(s_character_state_label, 22, 42);
    if (s_font_24 != nullptr) {
        lv_obj_set_style_text_font(s_character_state_label, s_font_24, 0);
    }
    lv_obj_set_style_text_color(s_character_state_label, lv_color_hex(0x4f455c), 0);

    /* Thinking-only wait animation in the stage's upper-right corner. */
    s_thinking_spinner = lv_spinner_create(right_panel);
    lv_obj_set_pos(s_thinking_spinner, 338, 18);
    lv_obj_set_size(s_thinking_spinner, 46, 46);
    lv_spinner_set_anim_params(s_thinking_spinner, 900, 250);
    lv_obj_set_style_arc_width(s_thinking_spinner, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_thinking_spinner, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_thinking_spinner, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_thinking_spinner, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_thinking_spinner, lv_color_hex(0xff6fa7), LV_PART_INDICATOR);
    make_non_interactive(s_thinking_spinner);
    lv_obj_add_flag(s_thinking_spinner, LV_OBJ_FLAG_HIDDEN);

    s_thinking_hint = lv_label_create(right_panel);
    lv_label_set_text(s_thinking_hint, "WAIT");
    lv_obj_set_pos(s_thinking_hint, 342, 66);
    lv_obj_set_style_text_font(s_thinking_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_thinking_hint, lv_color_hex(0x8c789a), 0);
    lv_obj_add_flag(s_thinking_hint, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *caption_card = lv_obj_create(right_panel);
    lv_obj_set_pos(caption_card, 20, 88);
    lv_obj_set_size(caption_card, 364, 68);
    style_card(caption_card, lv_color_hex(0xffffff), 18);
    lv_obj_set_style_bg_opa(caption_card, LV_OPA_80, 0);
    make_non_interactive(caption_card);

    lv_obj_t *caption_accent = lv_obj_create(caption_card);
    lv_obj_set_pos(caption_accent, 14, 14);
    lv_obj_set_size(caption_accent, 5, 40);
    style_card(caption_accent, lv_color_hex(0xff83af), 3);
    make_non_interactive(caption_accent);

    s_character_caption = lv_label_create(caption_card);
    lv_label_set_text(s_character_caption, "正在初始化设备服务");
    lv_obj_set_pos(s_character_caption, 32, 18);
    lv_obj_set_width(s_character_caption, 314);
    if (s_font_24 != nullptr) {
        lv_obj_set_style_text_font(s_character_caption, s_font_24, 0);
    }
    lv_obj_set_style_text_color(s_character_caption, lv_color_hex(0x5b4f64), 0);

    lv_obj_t *ground_shadow = lv_obj_create(right_panel);
    lv_obj_set_pos(ground_shadow, 34, 638);
    lv_obj_set_size(ground_shadow, 336, 30);
    style_card(ground_shadow, lv_color_hex(0x8e78a6), LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(ground_shadow, LV_OPA_20, 0);
    make_non_interactive(ground_shadow);

    if (characters_ready) {
        s_character_image = lv_image_create(right_panel);
        lv_image_set_src(s_character_image, &s_character_dsc[UI_CHARACTER_IDLE]);
        lv_obj_set_pos(s_character_image, 2, 151);
        make_non_interactive(s_character_image);
    }

    /* Keep labels and spinner above the character layer. */
    lv_obj_move_foreground(caption_card);
    lv_obj_move_foreground(s_thinking_spinner);
    lv_obj_move_foreground(s_thinking_hint);

    update_audio_visual();
    update_phase_visual(agent_app_phase(), false);
    update_metrics();

    otool_lvgl_port_unlock();

    if (s_audio_command_sem != nullptr && s_speech_text_lock != nullptr &&
        s_audio_task == nullptr) {
        BaseType_t created = xTaskCreate(
            audio_capture_task,
            "ui_audio_capture",
            CONFIG_OTOOL_SPEECH_APP_TASK_STACK_SIZE,
            nullptr,
            5,
            &s_audio_task);
        if (created != pdPASS) {
            s_audio_error.store(ESP_ERR_NO_MEM);
            s_audio_state.store(UI_AUDIO_UNAVAILABLE);
            ESP_LOGE(TAG, "audio interaction task create failed");
        }
    }

    lv_timer_create(ui_timer_cb, CONFIG_OTOOL_LLM_UI_REFRESH_MS, nullptr);
}
