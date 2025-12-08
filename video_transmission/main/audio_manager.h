// audio_manager.h
#ifndef AUDIO_MANAGER_H
#define AUDIO_MANAGER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// One-time initialization: create mutex and initialize codec to BOTH+START
void audio_manager_init(void);

// WakeNet management
esp_err_t audio_manager_start_wakenet(void);
esp_err_t audio_manager_stop_wakenet(void);

// Volume control
esp_err_t audio_manager_get_volume(int *out_vol);
esp_err_t audio_manager_set_volume(int vol);

// Play MP3 audio from memory buffer
esp_err_t audio_manager_play_from_flash(const uint8_t *start, const uint8_t *end);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_MANAGER_H
