#include "audio_manager.h"
#include "audio_play.h"

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


static audio_player_t s_player;
static bool s_player_inited = false;
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

esp_err_t audio_manager_play_from_flash(const uint8_t *start, const uint8_t *end)
{
    if (!start || !end || start >= end) {
        return ESP_ERR_INVALID_ARG;
    }

    audio_lock();

    // 1) If WakeNet running, stop it FIRST so it releases I2S
    bool resume_wakenet = s_wakenet_running;

    if (s_wakenet_running) {
        ESP_LOGI(TAG, "Stopping WakeNet for playback");
        wakenet_stop();
        s_wakenet_running = false;
    }

    // 2) Always recreate audio_player so we get a fresh I2S handle


    if (!s_player_inited) {
        ESP_LOGI(TAG, "Init audio_player (first time)");
        if (audio_player_init(&s_player) != 0) {
            ESP_LOGE(TAG, "audio_player_init failed");
            audio_unlock();
            return ESP_FAIL;
        }
        s_player_inited = true;
    }

    s_playback_running = true;
    audio_unlock();

    // 3) Do actual playback (this blocks until done)
    int r = audio_player_play_from_flash(&s_player, start, end);

    audio_lock();
    s_playback_running = false;

    if (s_player_inited) {
        ESP_LOGI(TAG, "Deinit audio_player after playback");
        audio_player_deinit(&s_player);
        s_player_inited = false;
    }

    // 4) Resume WakeNet if needed
    if (resume_wakenet) {
        ESP_LOGI(TAG, "Restarting WakeNet after playback");
        esp_err_t wr = wakenet_start();
        if (wr == ESP_OK) {
            s_wakenet_running = true;
        } else {
            ESP_LOGE(TAG, "Failed to restart WakeNet: %d", wr);

        }
    }

    audio_unlock();

    return (r == 0) ? ESP_OK : ESP_FAIL;
}