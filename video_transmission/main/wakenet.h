// wakenet.h
#ifndef WAKENET_H
#define WAKENET_H

#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Audio sampling configuration
#define SR_RATE_HZ                 16000
#define CODEC_ADC_I2S_PORT         0

// Recording parameters
#define RECORD_SECONDS             5
#define BYTES_PER_SECOND           (SR_RATE_HZ * 2)
#define RECORD_BYTES               (RECORD_SECONDS * BYTES_PER_SECOND)

// Network port for recording transmission
#define RECORDING_PORT             1000

// Startup delay before accepting wakeword detections (ms)
#define SECONDS_BEFORE_START       3000

// Start WakeNet engine and audio recording
esp_err_t wakenet_start(void);

// Stop WakeNet engine and cleanup resources
void      wakenet_stop(void);

#ifdef __cplusplus
}
#endif

#endif // WAKENET_H
