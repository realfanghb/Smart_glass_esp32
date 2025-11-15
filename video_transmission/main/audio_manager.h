#ifndef AUDIO_MANAGER_H
#define AUDIO_MANAGER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 一次性初始化：创建互斥锁 + 初始化 codec 为 BOTH+START
void audio_manager_init(void);

// WakeNet 管理（建议用这两个而不是直接调 wakenet_xxx）
esp_err_t audio_manager_start_wakenet(void);
esp_err_t audio_manager_stop_wakenet(void);

esp_err_t audio_manager_get_volume(int *out_vol);
esp_err_t audio_manager_set_volume(int vol);

esp_err_t audio_manager_play_from_flash(const uint8_t *start, const uint8_t *end);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_MANAGER_H
