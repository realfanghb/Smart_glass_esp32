#pragma once
#include <stdint.h>
#include "audio_pipeline.h"
#include "audio_element.h"
#include "audio_event_iface.h"
#include "board.h"

typedef struct {
    audio_pipeline_handle_t pipeline;
    audio_element_handle_t  mp3_decoder;
    audio_element_handle_t  i2s_writer;
    audio_event_iface_handle_t evt;
    audio_board_handle_t    board;
} audio_player_t;

#ifdef __cplusplus
extern "C" {
#endif

// 初始化板卡与流水线（ES8311 进入解码模式）
int audio_player_init(audio_player_t *player);

// A) 从“内嵌 MP3 二进制”播放（start/end 来自链接符号）
int audio_player_play_from_flash(audio_player_t *player,
                                 const uint8_t *start, const uint8_t *end);

// B) 从“文件路径”播放（例如 /sdcard/foo.mp3）
int audio_player_play_file_path(audio_player_t *player, const char *path);

// 释放资源
void audio_player_deinit(audio_player_t *player);

#ifdef __cplusplus
}
#endif