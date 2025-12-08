// ov2640_init.h
#ifndef OV2640_INIT_H
#define OV2640_INIT_H

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// GPIO pin definitions for OV2640 camera on KORVO-2
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     40

#define SIOD_GPIO_NUM     17  // SCCB SDA
#define SIOC_GPIO_NUM     18  // SCCB SCL

// DVP data lines: OV2640 D2..D9 mapped to esp32-camera d0..d7
#define Y2_GPIO_NUM       13
#define Y3_GPIO_NUM       47
#define Y4_GPIO_NUM       14
#define Y5_GPIO_NUM        3
#define Y6_GPIO_NUM       12
#define Y7_GPIO_NUM       42
#define Y8_GPIO_NUM       41
#define Y9_GPIO_NUM       39

#define VSYNC_GPIO_NUM    21
#define HREF_GPIO_NUM     38
#define PCLK_GPIO_NUM     11

// Initialize OV2640 for QVGA JPEG capture
// Returns ESP_OK on success
esp_err_t camera_init_ov2640(void);

#ifdef __cplusplus
}
#endif

#endif // OV2640_INIT_H
