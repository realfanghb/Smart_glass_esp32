idf_component_register(SRCS "main.c"  "ov2640_init.c" "softAP.c" "audio_play.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi esp_timer nvs_flash esp_netif esp32-camera audio_pipeline audio_board audio_hal esp_peripherals audio_stream)