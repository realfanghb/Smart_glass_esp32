set(srcs
    main.c
    speech_commands_action.c
    )

set(requires
    hardware_driver
    esp_wifi
    esp_netif
    esp_event
    nvs_flash
    lwip
    esp_timer
    )

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS include
                       REQUIRES ${requires})

component_compile_options(-w)