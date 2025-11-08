idf_component_register(SRCS "main.c"  "ov2640_init.c" "softAP.c" "audio_play.c" "wakenet.c" "audio_manager.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp-sr esp_wifi esp_timer nvs_flash esp_netif esp32-camera audio_pipeline audio_recorder audio_board audio_hal esp_peripherals audio_stream)