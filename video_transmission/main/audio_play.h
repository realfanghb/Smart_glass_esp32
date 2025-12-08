// audio_play.h
#ifndef AUDIO_PLAY_H
#define AUDIO_PLAY_H

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

// Initialize audio player pipeline (MP3 decoder -> I2S output)
int audio_player_init(audio_player_t *player);

// Play MP3 audio from memory buffer
int audio_player_play_from_flash(audio_player_t *player,
                                 const uint8_t *start, const uint8_t *end);

// Cleanup and deallocate audio player resources
void audio_player_deinit(audio_player_t *player);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_PLAY_H
