#include <string.h>
#include "audio_play.h"
#include "esp_log.h"
#include "audio_mem.h"
#include "i2s_stream.h"
#include "mp3_decoder.h"
#include "audio_common.h"
#include "esp_peripherals.h"
#include "audio_element.h"

#include "audio_manager.h"
// 若要用 B 方案（从文件路径），引入 file_stream
#include "fatfs_stream.h"   // 或者 spiffs_stream.h，根据你的存储介质

static const char *TAG = "audio_player";

// ---------- A 方案：内嵌 MP3 的 read 回调 ----------
typedef struct {
    const uint8_t *start;
    const uint8_t *end;
    int pos;
} flash_src_t;

static int flash_read_cb(audio_element_handle_t el, char *buf, int len, TickType_t tmo, void *ctx)
{
    flash_src_t *src = (flash_src_t *)ctx;
    int remain = (int)(src->end - src->start) - src->pos;
    if (remain <= 0) return AEL_IO_DONE;
    int n = (len < remain) ? len : remain;
    memcpy(buf, src->start + src->pos, n);
    src->pos += n;
    return n;
}

// ---------- 工具：等待播放结束 ----------
static void wait_until_finished(audio_pipeline_handle_t pipeline, audio_element_handle_t sink_el)
{
    while (1) {
        audio_element_state_t st = audio_element_get_state(sink_el);
        if (st == AEL_STATE_FINISHED || st == AEL_STATE_STOPPED
            || st == AEL_STATE_ERROR || st == AEL_STATE_NONE) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ---------- 处理解码器上报的音乐信息（动态配置/监测 I2S 时钟） ----------
static void pump_music_info_and_update_clk(audio_player_t *player)
{
    audio_event_iface_msg_t msg;
    while (audio_event_iface_listen(player->evt, &msg, 0) == ESP_OK) {
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT &&
            msg.source == (void*)player->mp3_decoder &&
            msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO)
        {
            audio_element_info_t mi = {0};
            audio_element_getinfo(player->mp3_decoder, &mi);
            ESP_LOGI(TAG, "music info: %d Hz, %d bits, %d ch",
                     mi.sample_rates, mi.bits, mi.channels);
                     
         i2s_stream_set_clk(player->i2s_writer, 16000, 16, 1);
         if (mi.sample_rates != 16000 || mi.bits != 16 || mi.channels != 1) {
     	 	ESP_LOGW(TAG, "Incoming MP3 not 16k/16bit/mono; please resample upstream.");
 			}
        }
    }
}

// ---------- 公共接口 ----------
int audio_player_init(audio_player_t *p)
{
    memset(p, 0, sizeof(*p));

    // 2) Pipeline
    audio_pipeline_cfg_t pcfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    p->pipeline = audio_pipeline_init(&pcfg);
    mem_assert(p->pipeline);

    // 3) mp3 解码器
    mp3_decoder_cfg_t mcfg = DEFAULT_MP3_DECODER_CONFIG();
    p->mp3_decoder = mp3_decoder_init(&mcfg);
    audio_pipeline_register(p->pipeline, p->mp3_decoder, "mp3");

    // 4) I2S writer
#if defined CONFIG_ESP32_C3_LYRA_V2_BOARD
    i2s_stream_cfg_t icfg = I2S_STREAM_PDM_TX_CFG_DEFAULT();
#else
    i2s_stream_cfg_t icfg = I2S_STREAM_CFG_DEFAULT();
#endif
    icfg.type = AUDIO_STREAM_WRITER;
    //icfg.chan_cfg.id = I2S_NUM_1;
    p->i2s_writer = i2s_stream_init(&icfg);    
    audio_pipeline_register(p->pipeline, p->i2s_writer, "i2s");

    const char *link[2] = {"mp3", "i2s"};
    audio_pipeline_link(p->pipeline, link, 2);

    // 5) 事件总线（只为接收 MUSIC_INFO 用以设定 I2S 时钟）
    audio_event_iface_cfg_t ecfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    p->evt = audio_event_iface_init(&ecfg);
    audio_pipeline_set_listener(p->pipeline, p->evt);
    return 0;
}

int audio_player_play_from_flash(audio_player_t *p, const uint8_t *start, const uint8_t *end)
{
    flash_src_t src = {.start = start, .end = end, .pos = 0};
    audio_element_set_read_cb(p->mp3_decoder, flash_read_cb, &src);
    
    size_t len = (size_t)(end - start);
    ESP_LOGI(TAG, "Playing from flash: start=%p end=%p length=%u bytes",
             start, end, (unsigned)len);

    // ===== START PIPELINE =====
    ESP_LOGI(TAG, "Starting audio pipeline");
    // 启动播放
    audio_pipeline_run(p->pipeline);

    // 在前几帧内尝试抓取 MUSIC_INFO 并更新 I2S 时钟
    for (int i = 0; i < 40; ++i) { // ~2s 内快速抽水
        pump_music_info_and_update_clk(p);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // 等待结束
    //wait_until_finished(p->pipeline, p->i2s_writer);

    // ===== Debug: Print decoded audio info (freq, channels, bits) =====
    audio_element_info_t info = {0};
    if (audio_element_getinfo(p->mp3_decoder, &info) == ESP_OK) {
        ESP_LOGI(TAG,
                 "Decoded audio: sample_rate=%d Hz, channels=%d, bits=%d",
                 info.sample_rates, info.channels, info.bits);
    } else {
        ESP_LOGW(TAG, "Failed to retrieve audio info from decoder");
    }

    // ===== PROGRESS LOGGING DURING PLAYBACK =====
    int elem_idx = 0;
    while (1) {
        audio_element_state_t st = audio_element_get_state(p->i2s_writer);
        ESP_LOGI(TAG, "Playing element #%d (state=%d)", elem_idx++, st);

        if (st == AEL_STATE_FINISHED ||
            st == AEL_STATE_STOPPED  ||
            st == AEL_STATE_ERROR) {
            ESP_LOGI(TAG, "Detected end of playback (state=%d)", st);
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(200)); // log every 200 ms
    }

    // (Optional) if you still want to use your original helper somewhere else,
    // you can remove or comment out the next line:
    // wait_until_finished(p->pipeline, p->i2s_writer);

    // ===== SHUTDOWN SEQUENCE =====
    ESP_LOGI(TAG, "Stopping and resetting pipeline");
    
    audio_pipeline_stop(p->pipeline);
    audio_pipeline_wait_for_stop(p->pipeline);
    audio_pipeline_terminate(p->pipeline);
    audio_pipeline_reset_ringbuffer(p->pipeline);
    audio_pipeline_reset_elements(p->pipeline);
    audio_pipeline_change_state(p->pipeline, AEL_STATE_INIT);
    return 0;
}


void audio_player_deinit(audio_player_t *p)
{
    if (!p) return;

    if (p->pipeline) {
        // 1) 先把元素从 pipeline 注销掉，避免 pipeline_deinit 再去 destroy 它们
        if (p->i2s_writer) {
            audio_pipeline_unregister(p->pipeline, p->i2s_writer);
        }
        if (p->mp3_decoder) {
            audio_pipeline_unregister(p->pipeline, p->mp3_decoder);
        }

        // 2) 移除监听，再销毁 pipeline 自身
        audio_pipeline_remove_listener(p->pipeline);
        audio_pipeline_deinit(p->pipeline);
        p->pipeline = NULL;
    }

    // 3) 销毁事件接口
    if (p->evt) {
        audio_event_iface_destroy(p->evt);
        p->evt = NULL;
    }

    // 4) 最后单独销毁各个 element
    if (p->i2s_writer) {
        audio_element_deinit(p->i2s_writer);
        p->i2s_writer = NULL;
    }
    if (p->mp3_decoder) {
        audio_element_deinit(p->mp3_decoder);
        p->mp3_decoder = NULL;
    }

    // 5) 清空结构体（可选）
    memset(p, 0, sizeof(*p));
}

