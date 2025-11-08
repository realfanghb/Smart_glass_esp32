// wakenet.c — Minimal WakeNet-only feed for ESP-ADF (IDF v5.x)
// Pipeline: I2S (16 kHz, 16-bit, ONLY_LEFT) -> RAW
// Recorder SR (AFE + WakeNet) pulls from RAW and logs on wakeword.
//
// Kconfig (ESP-SR):
//   [*] Enable AFE SR
//   [*] Enable WakeNet (WWE)
//   Language: English (pick model)
// PSRAM recommended.

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"

#include "audio_element.h"
#include "audio_pipeline.h"
#include "i2s_stream.h"
#include "raw_stream.h"
#include "audio_recorder.h"
#include "recorder_sr.h"

#include "audio_hal.h"
#include "board.h"

#include "wakenet.h"

#define SR_RATE_HZ                 16000

#ifndef CODEC_ADC_I2S_PORT
#define CODEC_ADC_I2S_PORT         0
#endif

static const char *TAG = "wakenet_min";

// Handles
static audio_pipeline_handle_t   s_pipeline   = NULL;
static audio_element_handle_t    s_i2s_reader = NULL;
static audio_element_handle_t    s_raw        = NULL;
static audio_rec_handle_t        s_recorder   = NULL;

/* -------------------- ADF pipeline: I2S -> RAW -------------------- */
static esp_err_t build_capture_pipeline(void)
{
    audio_board_handle_t board = audio_board_init();
    if (!board) {
        ESP_LOGE(TAG, "audio_board_init failed");
        return ESP_FAIL;
    }
    // Start codec (mic path enabled by board BSP)
    audio_hal_ctrl_codec(board->audio_hal, AUDIO_HAL_CODEC_MODE_BOTH, AUDIO_HAL_CTRL_START);

    // --- I2S reader @ 16k, 16-bit, LEFT-only ---
    i2s_stream_cfg_t i2s_cfg =
        I2S_STREAM_CFG_DEFAULT_WITH_PARA(CODEC_ADC_I2S_PORT,
                                         SR_RATE_HZ,            // 16 kHz
                                         16,                    // 16-bit
                                         AUDIO_STREAM_READER);

    // IMPORTANT: set channel type on the CONFIG before init()
    i2s_stream_set_channel_type(&i2s_cfg, I2S_CHANNEL_TYPE_ONLY_LEFT);

    // Optionally enlarge ringbuffer to reduce underflow
    i2s_cfg.out_rb_size = 8 * 1024;

    s_i2s_reader = i2s_stream_init(&i2s_cfg);
    if (!s_i2s_reader) {
        ESP_LOGE(TAG, "i2s_stream_init failed");
        return ESP_FAIL;
    }
    audio_element_set_input_timeout(s_i2s_reader, portMAX_DELAY);

    // --- RAW for the SR engine to pull from ---
    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type        = AUDIO_STREAM_READER;
    raw_cfg.out_rb_size = 20 * 1024;
    s_raw = raw_stream_init(&raw_cfg);
    if (!s_raw) {
        ESP_LOGE(TAG, "raw_stream_init failed");
        return ESP_FAIL;
    }
    audio_element_set_output_timeout(s_raw, portMAX_DELAY);

    // --- Assemble pipeline ---
    audio_pipeline_cfg_t pl_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    s_pipeline = audio_pipeline_init(&pl_cfg);
    if (!s_pipeline) {
        ESP_LOGE(TAG, "audio_pipeline_init failed");
        return ESP_FAIL;
    }

    audio_pipeline_register(s_pipeline, s_i2s_reader, "i2s");
    audio_pipeline_register(s_pipeline, s_raw,        "raw");

    const char *link_tag[2] = {"i2s", "raw"};
    if (audio_pipeline_link(s_pipeline, link_tag, 2) != ESP_OK) {
        ESP_LOGE(TAG, "audio_pipeline_link failed");
        return ESP_FAIL;
    }

    if (audio_pipeline_run(s_pipeline) != ESP_OK) {
        ESP_LOGE(TAG, "audio_pipeline_run failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Capture pipeline started: I2S(16k/16-bit LEFT) -> RAW");
    return ESP_OK;
}

/* -------------------- Recorder SR glue: RAW -> AFE/WakeNet -------------------- */
static int input_cb_for_afe(int16_t *buffer, int buf_sz, void *user_ctx, TickType_t ticks)
{
    (void)user_ctx; (void)ticks;
    // Blocks until data available; returns bytes read (<= buf_sz)
    int r = raw_stream_read(s_raw, (char *)buffer, buf_sz);
    if (r <= 0) {
        ESP_LOGW("AFE_FEED", "raw_stream_read=%d (buf_sz=%d)", r, buf_sz);
    }
    return r;
}

static esp_err_t rec_engine_cb(audio_rec_evt_t *event, void *user_data)
{
    (void)user_data;

    if (event->type == AUDIO_REC_WAKEUP_START) {
        recorder_sr_wakeup_result_t *wr = (recorder_sr_wakeup_result_t *)event->event_data;

        ESP_LOGI(TAG, "Wakeword DETECTED (vol=%.2f, model=%d, word=%d)",
                 wr->data_volume, wr->wakenet_model_index, wr->wake_word_index);

        // DETECTED!!!!
        printf("Wakeword detected\n");
        fflush(stdout);
    }

    return ESP_OK;
}


/* -------------------- Public API -------------------- */
esp_err_t wakenet_start(void)
{
    if (s_recorder) {
        ESP_LOGW(TAG, "WakeNet already running");
        return ESP_OK;
    }

    // 1) Start the capture pipeline so RAW has data to serve
    ESP_ERROR_CHECK(build_capture_pipeline());

    // 2) Configure Recorder SR (AFE + WakeNet) — wakeword only.
    // Use MONO mic ("M"). If you later enable AEC with a ref channel, switch to "RM".
    char *audio_sr_input_fmt = (char *)"M";

    recorder_sr_cfg_t sr_cfg = DEFAULT_RECORDER_SR_CFG(audio_sr_input_fmt,
                                                       "model",           // model partition label
                                                       AFE_TYPE_SR,
                                                       AFE_MODE_HIGH_PERF);
    // AFE / WakeNet params
    sr_cfg.afe_cfg->memory_alloc_mode       = AFE_MEMORY_ALLOC_MORE_PSRAM;
    sr_cfg.afe_cfg->wakenet_init            = true;
    sr_cfg.afe_cfg->vad_mode                = VAD_MODE_4;
    sr_cfg.afe_cfg->aec_init                = false;           // no AEC now
    sr_cfg.afe_cfg->agc_mode                = AFE_MN_PEAK_NO_AGC;
    sr_cfg.afe_cfg->pcm_config.sample_rate  = SR_RATE_HZ;      // 16 kHz
    sr_cfg.afe_cfg->pcm_config.mic_num      = 1;
    sr_cfg.afe_cfg->pcm_config.ref_num      = 0;
    sr_cfg.afe_cfg->pcm_config.total_ch_num = 1;
    sr_cfg.multinet_init                    = false;           // wakeword only

    audio_rec_cfg_t rec_cfg = AUDIO_RECORDER_DEFAULT_CFG();
    rec_cfg.read      = (recorder_data_read_t)&input_cb_for_afe;
    rec_cfg.sr_handle = recorder_sr_create(&sr_cfg, &rec_cfg.sr_iface);
    rec_cfg.event_cb  = rec_engine_cb;
    rec_cfg.vad_off   = 800;

    s_recorder = audio_recorder_create(&rec_cfg);
	ESP_ERROR_CHECK(audio_recorder_wakenet_enable(s_recorder, true));
	ESP_ERROR_CHECK(audio_recorder_vad_check_enable(s_recorder, true));
	ESP_ERROR_CHECK(audio_recorder_trigger_start(s_recorder));
    if (!s_recorder) {
        ESP_LOGE(TAG, "audio_recorder_create failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "WakeNet started. Say \"Hi ESP\" …");
    return ESP_OK;
}

void wakenet_stop(void)
{
    if (s_recorder) {
        audio_recorder_destroy(s_recorder);
        s_recorder = NULL;
    }

    if (s_pipeline) {
        audio_pipeline_stop(s_pipeline);
        audio_pipeline_wait_for_stop(s_pipeline);
        audio_pipeline_terminate(s_pipeline);

        if (s_i2s_reader) audio_pipeline_unregister(s_pipeline, s_i2s_reader);
        if (s_raw)        audio_pipeline_unregister(s_pipeline, s_raw);

        audio_pipeline_deinit(s_pipeline);
        s_pipeline = NULL;
    }

    if (s_i2s_reader) { audio_element_deinit(s_i2s_reader); s_i2s_reader = NULL; }
    if (s_raw)        { audio_element_deinit(s_raw);        s_raw        = NULL; }

    ESP_LOGI(TAG, "WakeNet stopped");
}
