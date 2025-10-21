// ============================================================================
// File: main.c (最小示例) —— 你的工程里只需调用 start() 就行
// ============================================================================
#include "nvs_flash.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "ov2640_init.h"
#include "softAP.h"



void app_main(void)
{
// NVS
esp_err_t ret = nvs_flash_init();
if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
ESP_ERROR_CHECK(nvs_flash_erase());
ESP_ERROR_CHECK(nvs_flash_init());
}


// Wi-Fi SoftAP
wifi_init_softap();


// video
ESP_ERROR_CHECK(camera_init_ov2640());
softap_tcp_stream_start(0);

//reverse_audio
ESP_ERROR_CHECK(softap_audio_player_init());
softap_audio_server_start(5000);
}