/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>                 // memcpy, memset
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_board_init.h"
#include "speech_commands_action.h"
#include "model_path.h"
#include "esp_process_sdkconfig.h"

// ===== ADDED: networking + timing headers =====
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"              // ADDED: inet_pton
#include "esp_heap_caps.h"
#include "esp_timer.h"              // ADDED: for precise capture timing

// ===== ADDED: Wi-Fi bring-up (STA) and events =====
#include "nvs_flash.h"              // ADDED
#include "esp_event.h"              // ADDED
#include "esp_netif.h"              // ADDED
#include "esp_wifi.h"               // ADDED


#include "esp_heap_caps.h"

// ===== TCP & audio config =====
#ifndef CONFIG_TCP_TARGET_IP
#define CONFIG_TCP_TARGET_IP   "172.20.10.3"  // keep as provided; ensure routing/firewall allows it
#endif
#ifndef CONFIG_TCP_TARGET_PORT
#define CONFIG_TCP_TARGET_PORT 3333
#endif

#define AUDIO_SR_HZ        16000
#define AUDIO_BITS         16
#define AUDIO_CH           1
#define RECORD_SECONDS     5



#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"          // <<< ADDED: for EventGroupHandle_t

// ... your existing includes ...
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"

// ===== TCP & audio config (leave as-is, just ensure IP matches your Mac) =====
#ifndef CONFIG_TCP_TARGET_IP
#define CONFIG_TCP_TARGET_IP   "192.168.1.23"       // <<< CHANGED: put your Mac's LAN IP here
#endif
#ifndef CONFIG_TCP_TARGET_PORT
#define CONFIG_TCP_TARGET_PORT 3333
#endif

// ===== HARD-CODED Wi-Fi credentials (no menuconfig needed) =====
#define WIFI_SSID        "jack"            // 
#define WIFI_PASSWORD    "12345678"        // 
// If your network is OPEN (no password), leave WIFI_PASSWORD as "".

// ===== audio config (unchanged) =====
#define AUDIO_SR_HZ        16000
#define AUDIO_BITS         16
#define AUDIO_CH           1
#define RECORD_SECONDS     10

// ========================= Wi-Fi helper (uses hard-coded SSID/PASS) =========================
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect(); // auto-retry
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
        ESP_LOGI("WIFI", "Got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_and_wait_ip(void)
{
    // --- NVS init with graceful fallback (Option A) ---
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW("NVS", "NVS full or new version; erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    if (nvs_ret == ESP_ERR_NOT_FOUND) {
        // No 'nvs' partition in your partition table -> proceed without persistent storage.
        ESP_LOGW("NVS", "No NVS partition found; continuing with RAM-only Wi-Fi storage.");
        // Don't abort; we'll set WIFI_STORAGE_RAM after esp_wifi_init().
    } else {
        // Any other error is fatal; OK is fine.
        ESP_ERROR_CHECK(nvs_ret);
    }

    // --- Bring up TCP/IP & event loop ---
    ESP_ERROR_CHECK(esp_netif_init());                     // starts LwIP/tcpip thread
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // --- Wi-Fi driver ---
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wicfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wicfg));

    // If we had no NVS, force Wi-Fi to use volatile (RAM) storage so it doesn't try to touch NVS.
    if (nvs_ret == ESP_ERR_NOT_FOUND) {
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    }

    // --- Event handlers & config ---
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid,     WIFI_SSID,     sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, WIFI_PASSWORD, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = (strlen(WIFI_PASSWORD) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    // --- Wait for IP ---
    s_wifi_event_group = xEventGroupCreate();
    ESP_LOGI("WIFI", "Connecting to SSID \"%s\" ...", WIFI_SSID);
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
}


// ===== WAV helpers (unchanged) =====
static void make_wav_header(uint8_t *hdr, uint32_t pcm_bytes,
                            uint16_t num_channels, uint32_t sample_rate,
                            uint16_t bits_per_sample)
{
    uint32_t byte_rate   = sample_rate * num_channels * (bits_per_sample / 8);
    uint16_t block_align = num_channels * (bits_per_sample / 8);
    uint32_t riff_size   = 36 + pcm_bytes;

    memcpy(hdr + 0,  "RIFF", 4);
    memcpy(hdr + 4,  &riff_size, 4);
    memcpy(hdr + 8,  "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    uint32_t subchunk1_size = 16; memcpy(hdr + 16, &subchunk1_size, 4);
    uint16_t audio_format = 1;    memcpy(hdr + 20, &audio_format, 2);
    memcpy(hdr + 22, &num_channels, 2);
    memcpy(hdr + 24, &sample_rate, 4);
    memcpy(hdr + 28, &byte_rate, 4);
    memcpy(hdr + 32, &block_align, 2);
    memcpy(hdr + 34, &bits_per_sample, 2);
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &pcm_bytes, 4);
}

static esp_err_t send_wav_over_tcp(const char *host, uint16_t port,
                                   const int16_t *pcm, size_t samples,
                                   uint16_t num_channels, uint32_t sample_rate)
{
    uint8_t hdr[44];
    const uint16_t bps = AUDIO_BITS;
    const uint32_t pcm_bytes = samples * (bps/8) * num_channels;
    make_wav_header(hdr, pcm_bytes, num_channels, sample_rate, bps);

    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(port);
    if (inet_pton(AF_INET, host, &dest.sin_addr) != 1) return ESP_FAIL;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return ESP_FAIL;
    if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
        close(sock);
        return ESP_FAIL;
    }

    if (send(sock, hdr, sizeof(hdr), 0) != sizeof(hdr)) { close(sock); return ESP_FAIL; }

    size_t bytes_total = pcm_bytes, sent = 0;
    const uint8_t *data = (const uint8_t *)pcm;
    while (sent < bytes_total) {
        int n = send(sock, data + sent, bytes_total - sent, 0);
        if (n <= 0) { close(sock); return ESP_FAIL; }
        sent += n;
    }
    shutdown(sock, SHUT_RDWR);
    close(sock);
    ESP_LOGI("TCP", "WAV sent: %u bytes (PCM %u)",
             (unsigned)(bytes_total + sizeof(hdr)), (unsigned)bytes_total);
    return ESP_OK;
}

// Optional raw sender (kept)
static esp_err_t send_buffer_tcp(const char *host, uint16_t port, const uint8_t *data, size_t len)
{
    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(port);
    if (inet_pton(AF_INET, host, &dest.sin_addr) != 1) {
        ESP_LOGE("TCP", "Invalid IP address: %s", host);
        return ESP_FAIL;
    }
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { ESP_LOGE("TCP", "socket() failed"); return ESP_FAIL; }
    if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
        ESP_LOGE("TCP", "connect() failed");
        close(sock);
        return ESP_FAIL;
    }
    size_t sent = 0;
    while (sent < len) {
        int n = send(sock, data + sent, len - sent, 0);
        if (n <= 0) { ESP_LOGE("TCP", "send() failed"); close(sock); return ESP_FAIL; }
        sent += n;
    }
    shutdown(sock, SHUT_RDWR);
    close(sock);
    ESP_LOGI("TCP", "Sent %u bytes to %s:%u", (unsigned)len, host, port);
    return ESP_OK;
}

// ===== SR globals =====
int wakeup_flag = 0;                // unused but kept
static esp_afe_sr_iface_t *afe_handle = NULL;
static volatile int task_flag = 0;
srmodel_list_t *models = NULL;

// ===== feed task (unchanged) =====
void feed_Task(void *arg)
{
    esp_afe_sr_data_t *afe_data = arg;
    int audio_chunksize = afe_handle->get_feed_chunksize(afe_data);
    int nch = afe_handle->get_feed_channel_num(afe_data);
    int feed_channel = esp_get_feed_channel();
    assert(nch == feed_channel);
    int16_t *i2s_buff = malloc(audio_chunksize * sizeof(int16_t) * feed_channel);
    assert(i2s_buff);

    while (task_flag) {
        esp_get_feed_data(true, i2s_buff, audio_chunksize * sizeof(int16_t) * feed_channel);
        afe_handle->feed(afe_data, i2s_buff);
    }
    free(i2s_buff);
    vTaskDelete(NULL);
}

// ===== Capture 5s and send as WAV (ADDED logs for verification) =====
static void record_and_send(esp_afe_sr_iface_t *afe, esp_afe_sr_data_t *afe_data, int chunk_samples)
{
    const int total_samples  = AUDIO_SR_HZ * RECORD_SECONDS;     // 16000 * 5 = 80000
    const size_t bytes_total = total_samples * sizeof(int16_t);  // 160000 bytes

    int16_t *pcm = (int16_t *)heap_caps_malloc(bytes_total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm) pcm = (int16_t *)malloc(bytes_total);
    if (!pcm) {
        ESP_LOGE("CAP", "No memory for capture buffer (%u B)", (unsigned)bytes_total);
        return;
    }

    const int64_t t0 = esp_timer_get_time();                    // ADDED: timestamp start (us)
    int samples_captured = 0;

    while (samples_captured < total_samples) {
        afe_fetch_result_t *res = afe->fetch(afe_data);
        if (!res || res->ret_value == ESP_FAIL) {
            ESP_LOGE("CAP", "AFE fetch error during capture");
            break;
        }
        int to_copy = chunk_samples;
        if (samples_captured + to_copy > total_samples) {
            to_copy = total_samples - samples_captured;
        }
        memcpy(pcm + samples_captured, res->data, to_copy * sizeof(int16_t));
        samples_captured += to_copy;
    }

    const int64_t t1 = esp_timer_get_time();                    // ADDED: timestamp end (us)
    ESP_LOGI("CAP", "Captured %d/%d samples (~%d ms, expected %d ms), bytes=%u",
             samples_captured, total_samples,
             (int)((t1 - t0)/1000), RECORD_SECONDS * 1000,
             (unsigned)(samples_captured * sizeof(int16_t)));

    if (samples_captured > 0) {
        // Send as WAV so PC can save/play immediately
        send_wav_over_tcp(CONFIG_TCP_TARGET_IP, CONFIG_TCP_TARGET_PORT,
                          pcm, samples_captured, AUDIO_CH, AUDIO_SR_HZ);
    }
    free(pcm);
}

void detect_Task(void *arg)
{
    bool capture_armed = false;
    esp_afe_sr_data_t *afe_data = arg;
    int afe_chunksize = afe_handle->get_fetch_chunksize(afe_data);
    char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_ENGLISH);
    printf("multinet:%s\n", mn_name);
    esp_mn_iface_t *multinet = esp_mn_handle_from_name(mn_name);
    model_iface_data_t *model_data = multinet->create(mn_name, 6000);
    int mu_chunksize = multinet->get_samp_chunksize(model_data);
    esp_mn_commands_update_from_sdkconfig(multinet, model_data);
    assert(mu_chunksize == afe_chunksize);
    multinet->print_active_speech_commands(model_data);

    printf("------------detect start------------\n");
    while (task_flag) {
        afe_fetch_result_t* res = afe_handle->fetch(afe_data);
        if (!res || res->ret_value == ESP_FAIL) {
            printf("fetch error!\n");
            break;
        }

        if (res->wakeup_state == WAKENET_DETECTED) {
            printf("WAKEWORD DETECTED\n");
            multinet->clean(model_data);
            capture_armed = true;   // arm capture on wake
        }

        if (res->raw_data_channels > 1 && res->wakeup_state == WAKENET_CHANNEL_VERIFIED) {
            printf("AFE_FETCH_CHANNEL_VERIFIED, channel index: %d\n", res->trigger_channel_id);
            capture_armed = true;   // for multi-channel
        }

        if (capture_armed) {
            record_and_send(afe_handle, afe_data, afe_chunksize);
            afe_handle->enable_wakenet(afe_data);
            capture_armed = false;
            printf("\n-----------10s audio captured & sent; awaits to be woken again-----------\n");
            continue;               // skip Multinet this cycle
        }

        // (Multinet path omitted)
    }
    if (model_data) {
        multinet->destroy(model_data);
        model_data = NULL;
    }
    printf("detect exit\n");
    vTaskDelete(NULL);
}



void app_main(void)
{
    // 1) Models + board init (as you already have)
    models = esp_srmodel_init("model");
    ESP_ERROR_CHECK(esp_board_init(AUDIO_SR_HZ, AUDIO_CH, AUDIO_BITS));

    // 2) Bring up Wi-Fi and wait for IP (your working function)
    wifi_init_and_wait_ip();

    // 3) AFE init (as you already have)
    afe_config_t *afe_config = afe_config_init(esp_get_input_format(), models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    afe_handle = esp_afe_handle_from_config(afe_config);
    esp_afe_sr_data_t *afe_data = afe_handle->create_from_config(afe_config);
    afe_config_free(afe_config);

    // (Optional but recommended: reduce Wi-Fi jitter)
    esp_wifi_set_ps(WIFI_PS_NONE);

    // 4) Start SR tasks BEFORE returning
	ESP_LOGI("APP", "heap: free=%u, internal=%u, min_internal=%u",
	         (unsigned)esp_get_free_heap_size(),
	         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
	         (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    // ===== CHANGED: create tasks with stacks in PSRAM using xTaskCreateStaticPinnedToCore =====
	// Why: internal DRAM is tight; putting large stacks in PSRAM avoids xTaskCreate() failures.
	
	task_flag = 1;
	
	// (You can tune these; 6–8 KB works well for both tasks.)
	#define DETECT_STACK_BYTES   (8 * 1024)   // ADDED: detect task stack (in BYTES)
	#define FEED_STACK_BYTES     (8 * 1024)   // ADDED: feed task stack (in BYTES)
	
	// TCBs are tiny and must reside in internal RAM.
	// 'static' here gives them static storage duration even though we’re inside a function.
	static StaticTask_t detect_tcb;           // ADDED
	static StaticTask_t feed_tcb;             // ADDED
	
	// Allocate the *stacks* in PSRAM (8-bit capable). This is the key change.
	StackType_t *detect_stack = (StackType_t *)heap_caps_malloc(
	    DETECT_STACK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);    // ADDED
	StackType_t *feed_stack   = (StackType_t *)heap_caps_malloc(
	    FEED_STACK_BYTES,   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);    // ADDED
	
	if (!detect_stack || !feed_stack) {                               // ADDED: safety
	    ESP_LOGE("APP", "Failed to alloc task stacks in PSRAM (detect=%p feed=%p)",
	             detect_stack, feed_stack);
	    if (detect_stack) free(detect_stack);
	    if (feed_stack)   free(feed_stack);
	} else {
	    // IMPORTANT: xTaskCreateStatic expects stack *length in WORDS* (not bytes),
	    // so divide by sizeof(StackType_t).
	    TaskHandle_t th_detect = xTaskCreateStaticPinnedToCore(
	        detect_Task, "detect",
	        DETECT_STACK_BYTES / sizeof(StackType_t),   // ADDED: stack length in words
	        (void*)afe_data,                            // same parameter as before
	        5,                                          // priority same as before
	        detect_stack, &detect_tcb,                  // ADDED: PSRAM stack + internal TCB
	        tskNO_AFFINITY);                            // ADDED: let scheduler choose core
	
	    TaskHandle_t th_feed = xTaskCreateStaticPinnedToCore(
	        feed_Task, "feed",
	        FEED_STACK_BYTES / sizeof(StackType_t),     // ADDED
	        (void*)afe_data,
	        6,
	        feed_stack, &feed_tcb,                      // ADDED
	        tskNO_AFFINITY);                            // ADDED
	
	    if (!th_detect || !th_feed) {                   // ADDED: creation result check
	        ESP_LOGE("APP", "Failed to create tasks (static) detect=%p feed=%p",
	                 th_detect, th_feed);
	        // Optional cleanup if you want:
	        // free(detect_stack); free(feed_stack);
	    } else {
	        ESP_LOGI("APP", "Tasks created (static): detect=%p feed=%p",
	                 th_detect, th_feed);
	    }
}


    // It’s fine for app_main to return now; the tasks keep running.
}

