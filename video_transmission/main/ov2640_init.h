// ov2640_init.h
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file ov2640_init.h
 * @brief OV2640 camera initialization helper (DVP -> esp_camera).
 */

/**
 * @brief Initialize OV2640 with default settings (JPEG format, QVGA size).
 *
 * @return ESP_OK on success; otherwise an error code from esp_camera_init().
 */
esp_err_t camera_init_ov2640(void);

#ifdef __cplusplus
}
#endif
