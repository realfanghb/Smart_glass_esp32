#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "esp_err.h"

/**
 * @file softAP.h
 * @brief Wi-Fi SoftAP initialization and TCP service entry points.
 *
 * This module brings up ESP32-S3 as a SoftAP and exposes multiple TCP services:
 * - Video JPEG streaming (length-prefixed)
 * - Reverse audio: receive MP3 bytes and play via audio pipeline
 * - Haptic feedback control via PWM (command over TCP)
 * - Key listener for volume control
 */

/**
 * @brief Initialize and start Wi-Fi SoftAP (AP mode).
 *
 * Starts the SoftAP with the SSID/password defined in the implementation.
 * Call once at startup before starting any TCP services.
 */
void wifi_init_softap(void);

/**
 * @brief Start the video streaming TCP server by creating task 'video_transmitting'.
 *
 * Typical protocol: [4-byte big-endian length N] + [N bytes JPEG frame].
 *
 * @param port TCP port to listen on (e.g., 2000).
 */
void softap_video_start(uint16_t port);

/**
 * @brief Start the reverse-audio TCP server (MP3 receive + playback) by creating task 'mp3_server_task'.
 *
 * Jetson Nano base station pushes MP3 bytes to this port. The device buffers the payload
 * (in PSRAM) and plays it via the audio pipeline.
 *
 * @param port TCP port to listen on (e.g., 3000).
 */
void softap_reverse_audio_start(uint16_t port);

/**
 * @brief SoftAP reverse-audio player init hook (no-op).
 *
 * Kept for API compatibility with the reverse-audio playback service.
 * The actual audio pipeline/codec initialization is performed in @c audio_manager,
 * which also enforces serialized audio processing. Therefore, this function does
 * not (re)initialize the player to avoid interfering with the shared audio
 * resources.
 *
 * @return ESP_OK (initialization is owned by @c audio_manager).
 */
esp_err_t softap_audio_player_init(void);


/**
 * @brief Start the haptic feedback TCP server (command -> PWM).
 *
 * Example command format (implementation-defined): ASCII digits for L/R intensity,
 * mapped to PWM duty on configured GPIOs by creating task 'vibration_feedback'.
 *
 * @param port TCP port to listen on (e.g., 4000).
 */
void softap_feedback_start(uint16_t port);

/**
 * @brief Start the key listener task (e.g., volume up/down) by creating task 'key_volume_task'.
 *
 * Uses board-specific key input events and adjusts codec volume accordingly.
 */
void softap_keys_start(void);

/**
 * @brief Dump internal RAM usage for debugging (heap/DRAM).
 *
 * @param tag Label printed with the memory report.
 */
void dump_internal_mem(const char *tag);

/**
 * @brief Dump external PSRAM usage for debugging.
 *
 * @param tag Label printed with the memory report.
 */
void dump_spiram_stat(const char *tag);

#ifdef __cplusplus
}
#endif
