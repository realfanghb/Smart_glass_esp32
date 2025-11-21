#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wakenet_start(void);
void      wakenet_stop(void);

#ifdef __cplusplus
}
#endif
