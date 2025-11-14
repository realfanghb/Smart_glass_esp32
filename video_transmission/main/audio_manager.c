#include "audio_manager.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "board.h"
#include "audio_hal.h"
#include "wakenet.h"

static const char *TAG = "audio_mgr";

static SemaphoreHandle_t    s_audio_mutex = NULL;
static audio_board_handle_t s_board       = NULL;

static bool s_wakenet_running             = false;
static bool s_playback_running            = false;
static bool s_wakenet_paused_for_playback = false;

// ---------- 内部锁 ----------

static void audio_lock(void)
{
    if (s_audio_mutex == NULL) {
        s_audio_mutex = xSemaphoreCreateMutex();
        if (s_audio_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create audio mutex");
            return;
        }
    }
    xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
}

static void audio_unlock(void)
{
    if (s_audio_mutex) {
        xSemaphoreGive(s_audio_mutex);
    }
}

// ---------- 初始化：只在这里动 codec 一次 ----------

void audio_manager_init(void)
{
    audio_lock();

    if (!s_board) {
        s_board = audio_board_init();
        if (!s_board || !s_board->audio_hal) {
            ESP_LOGE(TAG, "audio_board_init failed");
            audio_unlock();
            return;
        }
        audio_hal_ctrl_codec(s_board->audio_hal,
                             AUDIO_HAL_CODEC_MODE_BOTH,
                             AUDIO_HAL_CTRL_START);
        ESP_LOGI(TAG, "Codec initialized once: MODE=BOTH, START");
    }

    audio_unlock();
}

// ---------- WakeNet 控制（只控 pipeline，不再重新 init codec） ----------

esp_err_t audio_manager_start_wakenet(void)
{
    audio_lock();
    esp_err_t ret = ESP_OK;

    if (s_playback_running) {
        ESP_LOGW(TAG, "Cannot start WakeNet while playback is running");
        ret = ESP_ERR_INVALID_STATE;
        goto out;
    }

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

out:
    audio_unlock();
    return ret;
}

esp_err_t audio_manager_stop_wakenet(void)
{
    audio_lock();

    if (s_wakenet_running) {
        ESP_LOGI(TAG, "Stopping WakeNet");
        wakenet_stop();
        s_wakenet_running = false;
    }

    audio_unlock();
    return ESP_OK;
}

esp_err_t audio_manager_get_volume(int *out_vol)
{
    if (!out_vol) return ESP_ERR_INVALID_ARG;
    if (!s_board) return ESP_ERR_INVALID_STATE;

    audio_lock();
    int v = 0;
    esp_err_t ret = audio_hal_get_volume(s_board->audio_hal, &v);
    audio_unlock();

    if (ret == ESP_OK) {
        *out_vol = v;
    }
    return ret;
}

esp_err_t audio_manager_set_volume(int vol)
{
    if (!s_board) return ESP_ERR_INVALID_STATE;
    if (vol < 0)   vol = 0;
    if (vol > 100) vol = 100;

    audio_lock();
    esp_err_t ret = audio_hal_set_volume(s_board->audio_hal, vol);
    audio_unlock();

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Volume set to %d%%", vol);
    }
    return ret;
}