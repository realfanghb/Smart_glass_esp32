// softAP.h
#ifndef SOFTAP_H
#define SOFTAP_H

#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "esp_err.h"
#include "driver/ledc.h"

// WiFi AP configuration
#define WIFI_SSID       "fanghb"
#define WIFI_PASS       "eecs473_15"
#define WIFI_CHANNEL    6
#define MAX_STA_CONN    4

// Network service ports
#define VIDEO_PORT          2000
#define REVERSE_AUDIO_PORT  3000
#define CONTROL_PORT        4000

// Audio buffer limits
#define MAX_INMEM_BYTES (1 * 1024 * 1024)
#define RECV_CHUNK      (4 * 1024)
#define CONTROL_RX_BUFSZ 64

// Haptic PWM configuration
#define HAPTIC_PWM_GPIO_L       4
#define HAPTIC_PWM_GPIO_R       7
#define HAPTIC_PWM_TIMER        LEDC_TIMER_0
#define HAPTIC_PWM_MODE         LEDC_LOW_SPEED_MODE
#define HAPTIC_PWM_RES          LEDC_TIMER_10_BIT
#define HAPTIC_PWM_CH_L         LEDC_CHANNEL_0
#define HAPTIC_PWM_CH_R         LEDC_CHANNEL_1
#define ENABLE_HAPTIC_PWM       1

// Initialize WiFi SoftAP
void wifi_init_softap(void);

// Start MJPEG video streaming
void softap_video_start(uint16_t port);

// Start MP3 audio reception server
void softap_reverse_audio_start(uint16_t port);

// Initialize audio player for playback
esp_err_t softap_audio_player_init(void);

// Start haptic feedback control server
void softap_feedback_start(uint16_t port);

// Start key/button volume control task
void softap_keys_start(void);

// Print internal RAM statistics
void dump_internal_mem(const char *tag);

#ifdef __cplusplus
}
#endif

#endif // SOFTAP_H
