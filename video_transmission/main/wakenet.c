// wakenet.c
// WakeNet engine with automatic audio recording and TCP transmission
// Pipeline: I2S (16 kHz, 16-bit mono) -> RAW stream -> AFE/WakeNet
#include <string.h>
#include <stdlib.h>
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
#include "esp_timer.h"
#include "audio_hal.h"
#include "board.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"

#include "wakenet.h"
#include "softAP.h"


static const char *TAG = "wakenet_min";
static int64_t s_start_time_ms = 0;

static audio_pipeline_handle_t   s_pipeline   = NULL;
static audio_element_handle_t    s_i2s_reader = NULL;
static audio_element_handle_t    s_raw        = NULL;
static audio_rec_handle_t        s_recorder   = NULL;

static uint8_t *s_record_buf   = NULL;
static size_t   s_record_len   = 0;
static bool     s_is_recording = false;

static esp_err_t build_capture_pipeline(void)
{
    i2s_stream_cfg_t i2s_cfg =
        I2S_STREAM_CFG_DEFAULT_WITH_PARA(CODEC_ADC_I2S_PORT,
                                         SR_RATE_HZ,
                                         16,
                                         AUDIO_STREAM_READER);

    i2s_stream_set_channel_type(&i2s_cfg, I2S_CHANNEL_TYPE_ONLY_LEFT);
    i2s_cfg.out_rb_size = 8 * 1024;

    s_i2s_reader = i2s_stream_init(&i2s_cfg);
    if (!s_i2s_reader) {
        ESP_LOGE(TAG, "i2s_stream_init failed");
        return ESP_FAIL;
    }
    audio_element_set_input_timeout(s_i2s_reader, portMAX_DELAY);

    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type        = AUDIO_STREAM_READER;
    raw_cfg.out_rb_size = 20 * 1024;
    s_raw = raw_stream_init(&raw_cfg);
    if (!s_raw) {
        ESP_LOGE(TAG, "raw_stream_init failed");
        return ESP_FAIL;
    }
    audio_element_set_output_timeout(s_raw, portMAX_DELAY);

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

    ESP_LOGI(TAG, "Capture pipeline started: I2S(16k/16-bit) -> RAW");
    return ESP_OK;
}

static int input_cb_for_afe(int16_t *buffer, int buf_sz, void *user_ctx, TickType_t ticks)
{
    (void)user_ctx;
    (void)ticks;

    int r = raw_stream_read(s_raw, (char *)buffer, buf_sz);
    if (r <= 0) {
        ESP_LOGW("AFE_FEED", "raw_stream_read=%d (buf_sz=%d)", r, buf_sz);
    }
    return r;
}
static void send_recording_over_tcp(const uint8_t *data, size_t len)
{
    if (!data || len == 0) {
        ESP_LOGW(TAG, "send_recording_over_tcp: empty buffer");
        return;
    }

    ESP_LOGI(TAG, "Starting TCP server on port %d to send %u bytes",
             RECORDING_PORT, (unsigned)len);

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "recording socket() failed");
        return;
    }

    int yes = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(RECORDING_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "recording bind() failed");
        close(listen_sock);
        return;
    }

    if (listen(listen_sock, 1) < 0) {
        ESP_LOGE(TAG, "recording listen() failed");
        close(listen_sock);
        return;
    }

    ESP_LOGI(TAG, "Recording server listening on 0.0.0.0:%d", RECORDING_PORT);

    struct timeval tv = {
        .tv_sec  = 10,
        .tv_usec = 0,
    };
    setsockopt(listen_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in cli;
    socklen_t slen = sizeof(cli);
    int sock = accept(listen_sock, (struct sockaddr *)&cli, &slen);
    if (sock < 0) {
        ESP_LOGI(TAG, "No client connected to port 1000 within 10s");
        close(listen_sock);
		s_record_len = 0;
        return;
    }

    char ip[16];
    inet_ntoa_r(cli.sin_addr, ip, sizeof(ip));
    ESP_LOGI(TAG, "Recording client %s:%d connected", ip, ntohs(cli.sin_port));

    uint32_t nlen = htonl((uint32_t)len);
    int n = send(sock, &nlen, sizeof(nlen), 0);
    if (n != (int)sizeof(nlen)) {
        ESP_LOGE(TAG, "failed to send length header");
    } else {
        size_t off = 0;
        while (off < len) {
            int tosend = (int)(len - off);
            if (tosend > 4096) {
                tosend = 4096;
            }
            n = send(sock, data + off, tosend, 0);
            if (n <= 0) {
                ESP_LOGE(TAG, "send() failed at offset %u", (unsigned)off);
                break;
            }
            off += (size_t)n;
        }
        ESP_LOGI(TAG, "Recording sent: %u/%u bytes", (unsigned)off, (unsigned)len);
    }

    shutdown(sock, SHUT_RDWR);
    close(sock);
    close(listen_sock);

    ESP_LOGI(TAG, "Recording client disconnected (Port 1000)");
}

static void record_10s_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "recording task started");

    if (s_record_buf) {
        free(s_record_buf);
        s_record_buf = NULL;
    }

    s_record_buf = (uint8_t *)malloc(RECORD_BYTES);
    if (!s_record_buf) {
        ESP_LOGE(TAG, "Failed to allocate %d bytes for recording", (int)RECORD_BYTES);
        s_is_recording = false;
        vTaskDelete(NULL);
        return;
    }

    size_t offset = 0;
    const int chunk = BYTES_PER_SECOND / 5;
    const TickType_t t_start = xTaskGetTickCount();
    const TickType_t max_duration = pdMS_TO_TICKS(RECORD_SECONDS * 1000);  

    while (offset < RECORD_BYTES) {
        TickType_t now = xTaskGetTickCount();
        if (now - t_start > max_duration) {
            ESP_LOGI(TAG, "Max duration (%d seconds) reached, stopping recording.",
                     RECORD_SECONDS);
            break;
        }

        int wanted = chunk;
        if (wanted > (int)(RECORD_BYTES - offset)) {
            wanted = (int)(RECORD_BYTES - offset);
        }

        int r = audio_recorder_data_read(
                    s_recorder,
                    s_record_buf + offset,
                    wanted,
                    portMAX_DELAY);
        if (r <= 0) {
            ESP_LOGW(TAG, "audio_recorder_data_read returned %d, stopping early", r);
            break;
        }
        offset += (size_t)r;
    }
    s_record_len = offset;

    ESP_LOGI(TAG,
             "Recording done: %d bytes (~%.2f s at 16k/16-bit mono)",
             (int)s_record_len,
             (float)s_record_len / (float)BYTES_PER_SECOND);

    if (s_record_len > 0) {
        send_recording_over_tcp(s_record_buf, s_record_len);
    }

    UBaseType_t watermark = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "record_10s_task stack high water mark: %u words", (unsigned)watermark);

    s_is_recording = false;
    vTaskDelete(NULL);
}

static esp_err_t rec_engine_cb(audio_rec_evt_t *event, void *user_data)
{
    (void)user_data;

    if (event->type == AUDIO_REC_WAKEUP_START) {
        int64_t now = esp_timer_get_time() / 1000;
        if (now - s_start_time_ms < SECONDS_BEFORE_START) {
            return ESP_OK;
        }
        recorder_sr_wakeup_result_t *wr =
            (recorder_sr_wakeup_result_t *)event->event_data;

        ESP_LOGI(TAG, "Wakeword detected (vol=%.2f, model=%d, word=%d)",
                 wr->data_volume,
                 wr->wakenet_model_index,
                 wr->wake_word_index);

        if (!s_is_recording) {
            s_is_recording = true;
            dump_internal_mem(TAG);
            BaseType_t ret = xTaskCreate(
                record_10s_task,
                "rec10s",
                3072,      // stack size
                NULL,
                5,         // priority
                NULL
            );
            if (ret != pdPASS) {
                ESP_LOGE(TAG, "Failed to create record_10s_task");
				size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
				ESP_LOGE(TAG, "Failed to create record_10s_task, largest_int=%u", (unsigned)largest);
                s_is_recording = false;

            }
        }
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
	ESP_LOGI(TAG, "Using model partition label: \"%s\"", "model");  // <- 新增

    recorder_sr_cfg_t sr_cfg = DEFAULT_RECORDER_SR_CFG(audio_sr_input_fmt,
                                                       "model",           // model partition label
                                                       AFE_TYPE_SR,
                                                       AFE_MODE_LOW_COST);

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
    rec_cfg.vad_off   = 3000;

    s_recorder = audio_recorder_create(&rec_cfg);
    if (!s_recorder) {
        ESP_LOGE(TAG, "audio_recorder_create failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "WakeNet started. Say \"Hi ESP\" …");
    s_start_time_ms = esp_timer_get_time() / 1000;
	
    // Enable wakeword + VAD and start the recorder
    ESP_ERROR_CHECK(audio_recorder_wakenet_enable(s_recorder, true));
    ESP_ERROR_CHECK(audio_recorder_vad_check_enable(s_recorder, false));
    ESP_ERROR_CHECK(audio_recorder_trigger_start(s_recorder));

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

    if (s_i2s_reader) {
        audio_element_deinit(s_i2s_reader);
        s_i2s_reader = NULL;
    }
    if (s_raw) {
        audio_element_deinit(s_raw);
        s_raw = NULL;
    }

    if (s_record_buf) {
        free(s_record_buf);
        s_record_buf = NULL;
        s_record_len = 0;
        s_is_recording = false;
    }

    ESP_LOGI(TAG, "WakeNet stopped");
}