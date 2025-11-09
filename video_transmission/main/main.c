// ============================================================================
// File: main.c (最小示例) —— 你的工程里只需调用 start() 就行
// ============================================================================
#include "nvs_flash.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "ov2640_init.h"
#include "softAP.h"
#include "audio_manager.h"
#include "wakenet.h"
#include "audio_manager.h"



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
	
	// Init manager
    audio_manager_init();
	ESP_ERROR_CHECK(audio_manager_start_wakenet());
	

	// video
	// ESP_ERROR_CHECK(camera_init_ov2640());
	// softap_video_start(2000);

}