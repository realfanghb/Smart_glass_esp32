# INSIGHT: In-device Navigation and Scene Interpretation Glasses

Smart glasses firmware for low-vision navigation assistance (EECS 473 project).

## Features

- **Real-time Video Streaming**: OV2640 QVGA JPEG capture at port 2000
- **Voice Control**: WakeNet speech recognition (16 kHz, 5s recording on port 1000)
- **Audio Feedback**: MP3 playback with dynamic I2S clock adjustment
- **Haptic Feedback**: Dual PWM vibration motors on GPIO 4 & 7 (port 4000)
- **Volume Control**: Button-based audio volume adjustment
- **WiFi Connectivity**: SoftAP mode with 4-device max (port 3000 for audio reception)

## Architecture

```
ESP32-S3 (Korvo-2 Board)
├─ Camera (OV2640 DVP)
├─ Audio Input (I2S, 16kHz/16-bit) → WakeNet Engine
├─ Audio Output (MP3 Decoder → I2S → ES8311 Codec)
├─ WiFi SoftAP (SSID: fanghb)
└─ Haptic Motors (PWM dual-channel)
```

## Requirements

- **ESP-IDF**: 5.0.8
- **ESP-ADF**: Master branch

## Building

```bash
idf.py build
idf.py flash
idf.py monitor
```

## Network Services

| Service | Port | Protocol |
|---------|------|----------|
| Video (MJPEG) | 2000 | TCP |
| Audio (MP3) | 3000 | TCP |
| Wakeword Recording | 1000 | TCP |
| Haptic/Control | 4000 | TCP |

## License

Code provided as-is for EECS 473 course project. Uses Espressif IDF (Apache 2.0) and esp-adf components.
