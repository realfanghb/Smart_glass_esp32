#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "esp_err.h"

// 1) 启动 SoftAP
void wifi_init_softap(void);

// 2) （保留）启动 MJPEG 推流（若仍需要摄像头）
void softap_video_start(uint16_t port);

// 3) 启动 MP3 接收服务器（保持原函数名，便于兼容 main.c）
void softap_reverse_audio_start(uint16_t port);

// 4) 初始化音频播放器（使用 audio_play.c / ES8311 + ADF 管线）
esp_err_t softap_audio_player_init(void);

void softap_feedback_start(uint16_t port);

void softap_keys_start(void);
#ifdef __cplusplus
}
#endif
