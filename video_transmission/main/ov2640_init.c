// ov2640_init.c
// OV2640 camera initialization for ESP32-S3
// Configures DVP interface with QVGA JPEG output at 10 MHz clock frequency
#include "esp_camera.h"
#include "esp_log.h"
#include "esp_err.h"
#include "ov2640_init.h"

static const char *TAG = "ov2640_init";

esp_err_t camera_init_ov2640(void)
{
    camera_config_t config = {
        .pin_pwdn  = PWDN_GPIO_NUM,
        .pin_reset = RESET_GPIO_NUM,
        .pin_xclk = XCLK_GPIO_NUM,
        .pin_sccb_sda = SIOD_GPIO_NUM,
        .pin_sccb_scl = SIOC_GPIO_NUM,

        .pin_d7 = Y9_GPIO_NUM,
        .pin_d6 = Y8_GPIO_NUM,
        .pin_d5 = Y7_GPIO_NUM,
        .pin_d4 = Y6_GPIO_NUM,
        .pin_d3 = Y5_GPIO_NUM,
        .pin_d2 = Y4_GPIO_NUM,
        .pin_d1 = Y3_GPIO_NUM,
        .pin_d0 = Y2_GPIO_NUM,

        .pin_vsync = VSYNC_GPIO_NUM,
        .pin_href  = HREF_GPIO_NUM,
        .pin_pclk  = PCLK_GPIO_NUM,

        .xclk_freq_hz = 10000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,
        .frame_size   = FRAMESIZE_QVGA,
        .jpeg_quality = 12,
        .fb_count     = 2,
        .fb_location  = CAMERA_FB_IN_PSRAM,
        .grab_mode    = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: 0x%x", err);
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_framesize(s, FRAMESIZE_QVGA);
    }
    ESP_LOGI(TAG, "OV2640 init OK");
    return ESP_OK;
}
