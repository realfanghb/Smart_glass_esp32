#ifndef AUDIO_MANAGER_H
#define AUDIO_MANAGER_H

#include "esp_err.h"

// Initialize internal mutex/state. Call once in app_main().
void audio_manager_init(void);

// Start WakeNet (wakeword mode).
// If wakeword is already running, this is a no-op.
esp_err_t audio_manager_start_wakenet(void);

// Start audio playback (your SoftAP audio player).
// This will STOP WakeNet first, so only playback is active.
esp_err_t audio_manager_start_playback(void);

#endif // AUDIO_MANAGER_H
