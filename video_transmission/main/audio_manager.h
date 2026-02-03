#ifndef AUDIO_MANAGER_H
#define AUDIO_MANAGER_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file audio_manager.h
 * @brief System-level audio ownership and arbitration.
 *
 * Owns the audio board/codec lifecycle and provides a single entry point for:
 * - Codec bring-up and volume control
 * - Wake-word pipeline start/stop
 * - Flash-based playback with conflict-free handover (WakeNet <-> Playback)
 */

/**
 * @brief Initialize audio hardware once (board + codec).
 *
 * Initializes the audio board handle and starts the codec in full-duplex mode.
 * Safe to call multiple times; subsequent calls are no-ops after first success.
 *
 * Notes:
 * - This function does not start WakeNet or playback pipelines.
 * - Volume is restored to the last cached value (default 100%).
 */
void audio_manager_init(void);

/**
 * @brief Start the WakeNet keyword-spotting pipeline.
 *
 * Starts the microphone -> WakeNet processing pipeline.
 * Will refuse to start if playback is active (I2S conflict prevention).
 *
 * @return
 * - ESP_OK: started or already running
 * - ESP_ERR_INVALID_STATE: playback is running
 * - Other esp_err_t: propagated from 'wakenet_start' function in the WakeNet start routine
 */
esp_err_t audio_manager_start_wakenet(void);

/**
 * @brief Stop the WakeNet keyword-spotting pipeline.
 *
 * Stops WakeNet if running. No effect if already stopped.
 *
 * @return ESP_OK.
 */
esp_err_t audio_manager_stop_wakenet(void);

/**
 * @brief Get current codec volume (0–100).
 *
 * @param[out] out_vol Receives volume in percent.
 * @return
 * - ESP_OK on success
 * - ESP_ERR_INVALID_ARG if out_vol is NULL
 * - ESP_ERR_INVALID_STATE if audio_manager_init() has not completed
 * - Other esp_err_t from codec driver
 */
esp_err_t audio_manager_get_volume(int *out_vol);

/**
 * @brief Set codec volume (0–100).
 *
 * Input is clamped to [0, 100]. Updates both codec and internal cache so
 * future operations remain consistent.
 *
 * @param[in] vol Volume percent.
 * @return
 * - ESP_OK on success
 * - ESP_ERR_INVALID_STATE if audio_manager_init() has not completed
 * - Other esp_err_t from codec driver
 */
esp_err_t audio_manager_set_volume(int vol);

/**
 * @brief Play an audio payload from flash (blocking).
 *
 * Plays a contiguous byte range [start, end) (typically embedded MP3 data).
 * If WakeNet is running, it will be stopped before playback and restarted after.
 *
 * Concurrency:
 * - Internal state transitions are mutex-protected.
 * - The function may release the lock during the blocking playback to reduce
 *   system-wide contention, while still preventing illegal start/stop sequences.
 *
 * @param[in] start Pointer to first byte (inclusive).
 * @param[in] end   Pointer to end byte (exclusive).
 * @return
 * - ESP_OK on successful playback completion
 * - ESP_ERR_INVALID_ARG if range is invalid
 * - ESP_FAIL on playback/pipeline errors
 */
esp_err_t audio_manager_play_from_flash(const uint8_t *start, const uint8_t *end);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_MANAGER_H
