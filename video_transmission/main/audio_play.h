#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "audio_pipeline.h"
#include "audio_element.h"
#include "audio_event_iface.h"
#include "board.h"

/**
 * @file audio_play.h
 * @brief MP3 decode-and-playback pipeline wrapper.
 *
 * This module encapsulates an ESP-ADF audio pipeline for local MP3 playback:
 * - Source: TCP-received MP3 byte region linked into flash
 * - Decode: MP3 decoder element
 * - Sink: I2S stream writer
 *
 * The implementation listens for decoder MUSIC_INFO events and configures the
 * I2S clock for a fixed target format (16 kHz / 16-bit / mono). Audio
 * will be resampled accordingly to avoid format mismatch.
 */

/**
 * @brief Audio player instance (pipeline + elements + event interface).
 */
typedef struct {
    audio_pipeline_handle_t    pipeline;     /*!< Audio pipeline handle. */
    audio_element_handle_t     mp3_decoder;  /*!< MP3 decoder element. */
    audio_element_handle_t     i2s_writer;   /*!< I2S sink (stream writer). */
    audio_event_iface_handle_t evt;          /*!< Pipeline event interface. */
    audio_board_handle_t       board;        /*!< Board handle.*/
} audio_player_t;

/**
 * @brief Initialize the MP3->I2S playback pipeline.
 *
 * Creates and links pipeline elements (MP3 decoder + I2S writer) and attaches
 * an event listener for stream metadata. This call prepares the player for
 * playback but does not start audio output.
 *
 * @param player Player instance to initialize.
 * @return 0 on success (implementation may assert on allocation failure).
 */
int audio_player_init(audio_player_t *player);

/**
 * @brief Play an embedded MP3 payload from a memory region.
 *
 * The MP3 payload is provided as a [start, end) byte range. The function installs a read callback on the decoder,
 * starts the pipeline, configures the I2S clock based on MUSIC_INFO events, then
 * blocks until the sink reaches a terminal state. Finally, it stops and resets
 * the pipeline to an INIT-ready state.
 *
 * @param player Player instance.
 * @param start  Start address of MP3 byte region (inclusive).
 * @param end    End address of MP3 byte region (exclusive).
 * @return 0 on completion (terminal state reached).
 */
int audio_player_play_from_flash(audio_player_t *player,
                                 const uint8_t *start,
                                 const uint8_t *end);

/**
 * @brief Deinitialize and release all player resources.
 *
 * Unregisters elements, detaches or destroys the event interface, deinitializes
 * pipeline and elements, and clears the player instance.
 *
 * @param player Player instance.
 */
void audio_player_deinit(audio_player_t *player);

#ifdef __cplusplus
}
#endif
