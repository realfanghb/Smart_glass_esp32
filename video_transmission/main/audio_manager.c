#include "audio_manager.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "wakenet.h"   // wakenet_start(), wakenet_stop()
#include "softAP.h"    // wifi_init_softap(), softap_audio_player_init(), softap_reverse_audio_start(), softap_feedback_start()

static const char *TAG = "audio_mgr";

// Mutex and simple state flags
static SemaphoreHandle_t s_audio_mutex = NULL;
static bool s_wakenet_running = false;
static bool s_playback_running = false;

void audio_manager_init(void)
{
    if (s_audio_mutex == NULL) {
        s_audio_mutex = xSemaphoreCreateMutex();
        if (s_audio_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create audio mutex");
        }
    }
}

// Internal helper just to make locking nicer
static void audio_lock(void)
{
    if (s_audio_mutex == NULL) {
        audio_manager_init();
    }
    if (s_audio_mutex) {
        xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
    }
}

static void audio_unlock(void)
{
    if (s_audio_mutex) {
        xSemaphoreGive(s_audio_mutex);
    }
}

esp_err_t audio_manager_start_wakenet(void)
{
    audio_lock();

    esp_err_t ret = ESP_OK;

    if (!s_wakenet_running) {
        ESP_LOGI(TAG, "Starting WakeNet");
        ret = wakenet_start();
        if (ret == ESP_OK) {
            s_wakenet_running = true;
        } else {
            ESP_LOGE(TAG, "wakenet_start failed: %d", ret);
        }
    } else {
        ESP_LOGI(TAG, "WakeNet already running");
    }

    audio_unlock();
    return ret;
}

esp_err_t audio_manager_start_playback(void)
{
    audio_lock();

    esp_err_t ret = ESP_OK;

    // If WakeNet is running, stop it first so we don't fight over I2S/codec
    if (s_wakenet_running) {
        ESP_LOGI(TAG, "Stopping WakeNet before starting playback");
        wakenet_stop();
        s_wakenet_running = false;
    }

    if (!s_playback_running) {
        ESP_LOGI(TAG, "Starting audio playback pipeline");
        // Init audio player + start your reverse/feedback sounds
        ret = softap_audio_player_init();
        if (ret == ESP_OK) {
            softap_reverse_audio_start(3000);
            softap_feedback_start(4000);
            s_playback_running = true;
        } else {
            ESP_LOGE(TAG, "softap_audio_player_init failed: %d", ret);
        }
    } else {
        ESP_LOGI(TAG, "Playback already running");
    }

    audio_unlock();
    return ret;
}
