#include <string.h>
#include "audio_play.h"
#include "esp_log.h"
#include "audio_mem.h"
#include "i2s_stream.h"
#include "mp3_decoder.h"
#include "audio_common.h"
#include "esp_peripherals.h"
#include "audio_element.h"

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

// ---------- 处理解码器上报的音乐信息（动态配置 I2S 时钟） ----------
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
            i2s_stream_set_clk(player->i2s_writer, mi.sample_rates, mi.bits, mi.channels);
        }
    }
}

// ---------- 公共接口 ----------
int audio_player_init(audio_player_t *p)
{
    memset(p, 0, sizeof(*p));

    // 1) 板级与 ES8311
    p->board = audio_board_init();
    audio_hal_ctrl_codec(p->board->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);

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

    // 启动播放
    audio_pipeline_run(p->pipeline);

    // 在前几帧内尝试抓取 MUSIC_INFO 并更新 I2S 时钟
    for (int i = 0; i < 40; ++i) { // ~2s 内快速抽水
        pump_music_info_and_update_clk(p);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // 等待结束
    wait_until_finished(p->pipeline, p->i2s_writer);

    // 停止与复位（以便下次还能继续调用）
    audio_pipeline_stop(p->pipeline);
    audio_pipeline_wait_for_stop(p->pipeline);
    audio_pipeline_terminate(p->pipeline);
    audio_pipeline_reset_ringbuffer(p->pipeline);
    audio_pipeline_reset_elements(p->pipeline);
    audio_pipeline_change_state(p->pipeline, AEL_STATE_INIT);
    return 0;
}

int audio_player_play_file_path(audio_player_t *p, const char *path)
{
    // 用 FATFS stream 作为 reader（也可换成 spiffs_stream）
    fatfs_stream_cfg_t fcfg = FATFS_STREAM_CFG_DEFAULT();
    fcfg.type = AUDIO_STREAM_READER;
    audio_element_handle_t file_reader = fatfs_stream_init(&fcfg);

    // 把 file_reader 插到 mp3_decoder 之前
    audio_pipeline_unregister(p->pipeline, p->mp3_decoder);
    audio_pipeline_register(p->pipeline, file_reader,   "file");
    audio_pipeline_register(p->pipeline, p->mp3_decoder, "mp3");

    const char *link2[3] = {"file", "mp3", "i2s"};
    audio_pipeline_link(p->pipeline, link2, 3);

    // 打开目标文件
    audio_element_set_uri(file_reader, path);

    // 播放启动
    audio_pipeline_run(p->pipeline);

    // 捕获 MUSIC_INFO 并设定 I2S 时钟
    for (int i = 0; i < 40; ++i) {
        pump_music_info_and_update_clk(p);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // 等待播放完成
    wait_until_finished(p->pipeline, p->i2s_writer);

    // 停止并复位
    audio_pipeline_stop(p->pipeline);
    audio_pipeline_wait_for_stop(p->pipeline);
    audio_pipeline_terminate(p->pipeline);
    audio_pipeline_reset_ringbuffer(p->pipeline);
    audio_pipeline_reset_elements(p->pipeline);
    audio_pipeline_change_state(p->pipeline, AEL_STATE_INIT);

    // 恢复到无文件的 2 元素链（mp3->i2s），便于下次用 A 方案
    audio_pipeline_unregister(p->pipeline, file_reader);
    audio_pipeline_unregister(p->pipeline, p->mp3_decoder);
    audio_pipeline_register(p->pipeline, p->mp3_decoder, "mp3");
    const char *linkA[2] = {"mp3", "i2s"};
    audio_pipeline_link(p->pipeline, linkA, 2);
    audio_element_deinit(file_reader);
    return 0;
}

void audio_player_deinit(audio_player_t *p)
{
    if (!p) return;
    if (p->pipeline) {
        audio_pipeline_remove_listener(p->pipeline);
        audio_pipeline_deinit(p->pipeline);
    }
    if (p->evt) audio_event_iface_destroy(p->evt);
    if (p->i2s_writer) audio_element_deinit(p->i2s_writer);
    if (p->mp3_decoder) audio_element_deinit(p->mp3_decoder);
    if (p->board && p->board->audio_hal) {
        audio_hal_ctrl_codec(p->board->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_STOP);
    }
    memset(p, 0, sizeof(*p));
}
