#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"

/**
 * @file wakenet.h
 * @brief WakeNet wake word detection service (Recorder SR) with PCM capture + TCP send.
 *
 * This module starts an audio capture pipeline and a Recorder SR engine (AFE + WakeNet)
 * after triggering on-device wake word detection ("Hi ESP").
 *
 */

/**
 * @brief Start WakeNet (AFE + WakeNet) and the following audio capture pipeline.
 *
 * This function initializes:
 * - I2S reader and RAW stream pipeline (16 kHz / 16-bit / mono)
 * - Recorder SR with WakeNet enabled (wakeword-only; multinet disabled)
 * - WakeNet + VAD configuration, and starts the recorder trigger loop
 *
 * If WakeNet is already running, the function returns ESP_OK without re-initializing.
 *
 * @return
 * - ESP_OK on success (or if already started)
 * - ESP_FAIL / other esp_err_t codes on initialization failure
 */
esp_err_t wakenet_start(void);

/**
 * @brief Stop WakeNet and deinitialize all related resources.
 *
 * This function tears down:
 * - Recorder SR / audio recorder handle
 * - Audio pipeline and registered elements (I2S reader, RAW stream)
 * - Any allocated recording buffers and internal state
 *
 * Safe to call multiple times.
 */
void wakenet_stop(void);

#ifdef __cplusplus
}
#endif
