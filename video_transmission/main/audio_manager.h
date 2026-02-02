#ifndef AUDIO_MANAGER_H
#define AUDIO_MANAGER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @file audio_manager.h
 *
 * @brief High-level audio manager API for the ESP32 audio codec + WakeNet
 *        keyword spotting pipeline + flash-based playback.
 *
 * This module wraps:
 *   - Board / codec bring-up via audio_board_init() and audio_hal_*()
 *   - WakeNet pipeline start / stop (microphone -> keyword spotting)
 *   - Playback of pre-encoded audio stored in flash via audio_player_*
 *
 * Internally, the module:
 *   - Uses a FreeRTOS mutex to serialize access to the audio hardware
 *   - Ensures the codec is initialized exactly once
 *   - Avoids I2S conflicts by never running WakeNet and playback at
 *     the same time (playback temporarily stops WakeNet, then restores it)
 *
 * Thread-safety:
 *   All functions are safe to call from multiple tasks; the module
 *   serializes access with an internal mutex.
 */

/**
 * @brief Initialize the audio board and codec once.
 *
 * Responsibilities:
 *   - Calls audio_board_init() to obtain the audio_board_handle_t
 *   - Starts the codec in AUDIO_HAL_CODEC_MODE_BOTH
 *   - Applies the last known volume (default 100%)
 *
 * Usage:
 *   - Must be called before any other audio_manager_* API.
 *   - Safe to call multiple times; after the first successful call,
 *     subsequent calls are effectively no-ops.
 */
void audio_manager_init(void);


/**
 * @brief Start the WakeNet keyword-spotting pipeline.
 *
 * This function brings up the WakeNet audio pipeline that listens
 * to the microphone and runs keyword spotting. It does NOT re-init
 * the codec; it only starts the pipeline.
 *
 * Constraints:
 *   - If playback is currently running, this call fails with
 *     ESP_ERR_INVALID_STATE to prevent I2S conflicts.
 *   - If WakeNet is already running, this call succeeds but does
 *     nothing (idempotent).
 *
 * @return
 *   - ESP_OK on success or if already running
 *   - ESP_ERR_INVALID_STATE if playback is running
 *   - Other error codes propagated from wakenet_start()
 */
esp_err_t audio_manager_start_wakenet(void);
/**
 * @brief Stop the WakeNet keyword-spotting pipeline.
 *
 * This function stops the WakeNet pipeline (if it is running).
 * It does not touch the codec initialization state.
 *
 * Behavior:
 *   - If WakeNet is not running, this call is a no-op and returns ESP_OK.
 *
 * @return ESP_OK always (currently no error path is exposed).
 */
esp_err_t audio_manager_stop_wakenet(void);

/**
 * @brief Get the current codec volume (0–100%).
 *
 * This is a thin wrapper over audio_hal_get_volume() using the
 * internally stored audio_board_handle_t.
 *
 * @param[out] out_vol
 *     Pointer to an int that will receive the current volume (0–100).
 *
 * @return
 *   - ESP_OK on success and *out_vol is filled
 *   - ESP_ERR_INVALID_ARG if out_vol is NULL
 *   - ESP_ERR_INVALID_STATE if the audio board / codec has not
 *     been initialized (audio_manager_init() not yet called)
 *   - Other error codes propagated from audio_hal_get_volume()
 */
esp_err_t audio_manager_get_volume(int *out_vol);
/**
 * @brief Set the codec volume (0–100%).
 *
 * This function clamps the input to [0, 100], calls
 * audio_hal_set_volume(), and updates the internal cached volume.
 *
 * Constraints:
 *   - Requires that audio_manager_init() has successfully completed.
 *
 * @param[in] vol
 *     Desired volume in percent (values outside [0,100] are clamped).
 *
 * @return
 *   - ESP_OK on success
 *   - ESP_ERR_INVALID_STATE if the audio board / codec is not ready
 *   - Other error codes propagated from audio_hal_set_volume()
 */
esp_err_t audio_manager_set_volume(int vol);

/**
 * @brief Play an MP3 (or other supported format) from flash.
 *
 * The audio data is assumed to be stored contiguously in flash,
 * described by the [start, end) byte range.
 *
 * High-level behavior:
 *   1. Validates the input range (start < end, non-NULL).
 *   2. If WakeNet is running, stops it first so it releases I2S.
 *   3. Lazily initializes an internal audio_player_t instance
 *      (audio_player_init) if not already initialized.
 *   4. Sets the codec volume to the internally cached value.
 *   5. Performs blocking playback via audio_player_play_from_flash().
 *   6. Deinitializes the audio_player_t instance after playback.
 *   7. If WakeNet was running before playback, restarts it.
 *
 * Concurrency:
 *   - This function holds the audio manager mutex while manipulating
 *     shared state (flags, player handle) but releases it during the
 *     blocking playback call so other tasks can still query volume, etc.
 *
 * @param[in] start
 *     Pointer to the first byte of the audio data in flash.
 * @param[in] end
 *     Pointer one past the last byte of the audio data in flash.
 *
 * @return
 *   - ESP_OK on successful playback
 *   - ESP_ERR_INVALID_ARG if start/end are invalid (NULL or start >= end)
 *   - ESP_FAIL if audio_player_init() fails or if
 *     audio_player_play_from_flash() reports a non-zero error code
 */
esp_err_t audio_manager_play_from_flash(const uint8_t *start, const uint8_t *end);
#ifdef __cplusplus
}
#endif

#endif // AUDIO_MANAGER_H
