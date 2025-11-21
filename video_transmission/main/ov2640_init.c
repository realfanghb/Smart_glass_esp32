// ov2640_init.c
#include "esp_camera.h"
#include "esp_log.h"
#include "esp_err.h"
#include "ov2640_init.h"

static const char *TAG = "ov2640_init";

// ====== KORVO-2 DVP + OV2640 引脚映射（按你现在直连的那版） ======
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     40

#define SIOD_GPIO_NUM     17  // SCCB SDA
#define SIOC_GPIO_NUM     18  // SCCB SCL

// DVP数据线（OV2640 的 D2..D9 -> esp32-camera 的 d0..d7）
#define Y2_GPIO_NUM       13  // D2 -> d0
#define Y3_GPIO_NUM       47  // D3 -> d1
#define Y4_GPIO_NUM       14  // D4 -> d2
#define Y5_GPIO_NUM        3  // D5 -> d3
#define Y6_GPIO_NUM       12  // D6 -> d4
#define Y7_GPIO_NUM       42  // D7 -> d5
#define Y8_GPIO_NUM       41  // D8 -> d6
#define Y9_GPIO_NUM       39  // D9 -> d7

#define VSYNC_GPIO_NUM    21
#define HREF_GPIO_NUM     38
#define PCLK_GPIO_NUM     11

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

        .xclk_freq_hz = 10000000,              // 20MHz
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,        // 以 JPEG 发送
        .frame_size   = FRAMESIZE_QVGA,
        .jpeg_quality = 12,                    // 质量/码率折中
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
        // 可根据需要：
        // s->set_vflip(s, 1);
        // s->set_hmirror(s, 1);
        // s->set_brightness(s, 1);
    }
    ESP_LOGI(TAG, "OV2640 init OK (SVGA JPEG)");
    return ESP_OK;
}
