// ov2640_init.h
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 供 main.c 调用：初始化 OV2640 为 800x600 JPEG，返回 ESP_OK 表示成功
esp_err_t camera_init_ov2640(void);

#ifdef __cplusplus
}
#endif
