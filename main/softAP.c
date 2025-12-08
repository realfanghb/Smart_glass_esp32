// softAP.c
// ESP32-S3 SoftAP + MJPEG streaming + MP3 reception server
#include <string.h>
#include <inttypes.h>
#include "audio_manager.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_camera.h"
#include "esp_peripherals.h"
#include "periph_button.h"
#include "periph_touch.h"
#include "periph_adc_button.h"
#include "audio_event_iface.h"
#include "board.h"
#include "driver/ledc.h"

#include "softAP.h"
#include "audio_play.h"
#include "audio_manager.h"

static const char *TAG = "softap";

static audio_player_t s_player;

static TaskHandle_t s_video_task = NULL;
static TaskHandle_t s_mp3_task = NULL;
static TaskHandle_t s_vibration_task = NULL;
static TaskHandle_t s_key_task = NULL;

static bool s_haptic_pwm_inited = false;


void dump_internal_mem(const char *tag)
{
    size_t free_int    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t largest_int = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    ESP_LOGI(tag, "INT free=%u, largest=%u",
             (unsigned)free_int, (unsigned)largest_int);
}

void dump_spiram_stat(const char *tag)
{
    size_t free_spiram    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t largest_spiram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    ESP_LOGI(tag, "SPIRAM free=%u, largest=%u",
             (unsigned)free_spiram, (unsigned)largest_spiram);
}

void wifi_init_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = { 0 };
    strcpy((char*)wifi_config.ap.ssid, WIFI_SSID);
    wifi_config.ap.ssid_len = strlen(WIFI_SSID);
    wifi_config.ap.channel = WIFI_CHANNEL;
    strcpy((char*)wifi_config.ap.password, WIFI_PASS);
    wifi_config.ap.max_connection = MAX_STA_CONN;
    wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    wifi_config.ap.pmf_cfg.required = true;

    if (strlen(WIFI_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT40));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP ready. SSID:%s PASS:%s CH:%d (AP GW 192.168.4.1)",
             WIFI_SSID, WIFI_PASS, WIFI_CHANNEL);
}

static void stream_one_client(int sock)
{
    const int send_timeout_ms = 3000;
    struct timeval tv = { .tv_sec = send_timeout_ms/1000, .tv_usec = (send_timeout_ms%1000)*1000 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int64_t last_log = esp_timer_get_time();
    int frames = 0;

    while (1) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }

        uint32_t len = fb->len;
        uint8_t header[4] = {
            (uint8_t)((len >> 24) & 0xFF),
            (uint8_t)((len >> 16) & 0xFF),
            (uint8_t)((len >> 8) & 0xFF),
            (uint8_t)(len & 0xFF)
        };
        int n = send(sock, header, 4, 0);
        if (n != 4) { esp_camera_fb_return(fb); break; }

        int off = 0;
        while (off < (int)len) {
            int tosend = len - off;
            if (tosend > 16 * 1024) tosend = 16 * 1024;
            n = send(sock, fb->buf + off, tosend, 0);
            if (n <= 0) { off = -1; break; }
            off += n;
        }
        esp_camera_fb_return(fb);
        if (off < 0) break;

        frames++;
        int64_t now = esp_timer_get_time();
        if (now - last_log > 1000000) {
            ESP_LOGI(TAG, "stream fps=%d", frames);
            frames = 0;
            last_log = now;
        }
    }
}

static void video_transmitting(void *arg)
{
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) { vTaskDelete(NULL); return; }
    int yes = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(VIDEO_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(listen_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(listen_sock); vTaskDelete(NULL); return; }
    if (listen(listen_sock, 1) < 0) { close(listen_sock); vTaskDelete(NULL); return; }
    ESP_LOGI(TAG, "MJPEG listening on 0.0.0.0:%d", VIDEO_PORT);

    while (1) {
        struct sockaddr_in cli; socklen_t slen = sizeof(cli);
        int sock = accept(listen_sock, (struct sockaddr*)&cli, &slen);
        if (sock < 0) continue;
        char ip[16]; inet_ntoa_r(cli.sin_addr, ip, sizeof(ip));
        ESP_LOGI(TAG, "client %s:%d connected", ip, ntohs(cli.sin_port));
        stream_one_client(sock);
        ESP_LOGI(TAG, "client disconnected (Port 2000)");
        shutdown(sock, SHUT_RDWR); close(sock);
    }
}

void softap_video_start(uint16_t port)
{
    (void) port;
    if (s_video_task) {
        ESP_LOGI(TAG, "tcp stream task already running on port %d", VIDEO_PORT);
        return;
    }
    dump_internal_mem(TAG);
    xTaskCreate(video_transmitting, "video_transmitting", 5120, NULL, 5, &s_video_task);
}

esp_err_t softap_audio_player_init(void)
{
    ESP_LOGI(TAG, "softap_audio_player_init: no-op (audio_manager owns audio)");
    return ESP_OK;
}

static inline bool looks_like_mp3(const uint8_t *buf, size_t n)
{
    if (n >= 3 && buf[0]=='I' && buf[1]=='D' && buf[2]=='3') return true;
    if (n >= 2 && buf[0]==0xFF && (buf[1] & 0xE0) == 0xE0) return true;
    return false;
}

static void reverse_audio(void *arg)
{
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) { ESP_LOGE(TAG, "socket failed"); vTaskDelete(NULL); return; }
    int yes = 1; setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(REVERSE_AUDIO_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(listen_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind failed"); close(listen_sock); vTaskDelete(NULL); return;
    }
    if (listen(listen_sock, 1) < 0) {
        ESP_LOGE(TAG, "listen failed"); close(listen_sock); vTaskDelete(NULL); return;
    }
    ESP_LOGI(TAG, "MP3 server listening on 0.0.0.0:%d", REVERSE_AUDIO_PORT);

    dump_spiram_stat(TAG);
    uint8_t *chunk = (uint8_t*)heap_caps_malloc(RECV_CHUNK, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (!chunk) { ESP_LOGE(TAG, "recv chunk alloc failed"); close(listen_sock); vTaskDelete(NULL); return; }

    for (;;)
    {
        struct sockaddr_in cli; socklen_t sl = sizeof(cli);
        int sock = accept(listen_sock, (struct sockaddr*)&cli, &sl);
        if (sock < 0) continue;

        char ip[16]; inet_ntoa_r(cli.sin_addr, ip, sizeof(ip));
        ESP_LOGI(TAG, "Client %s:%d connected (audio in-memory playback)", ip, ntohs(cli.sin_port));

        dump_spiram_stat(TAG);
        uint8_t *mem = (uint8_t*)heap_caps_malloc(MAX_INMEM_BYTES, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
        if (!mem) {
            ESP_LOGE(TAG, "buffer alloc failed. Lower MAX_INMEM_BYTES or enable PSRAM.");
            shutdown(sock, SHUT_RDWR); close(sock);
            continue;
        }

        size_t filled = 0;
        bool header_checked = false, header_ok = false, ok = true;

        while (1) {
            int n = recv(sock, (char*)chunk, RECV_CHUNK, 0);
            if (n == 0) break;
            if (n < 0) { ok = false; break; }

            if (!header_checked) {
                header_ok = looks_like_mp3(chunk, (size_t)n);
                header_checked = true;
                if (!header_ok) {
                    ESP_LOGW(TAG, "incoming data doesn't look like MP3 (no ID3/sync)");
                }
            }

            if (filled + (size_t)n > MAX_INMEM_BYTES) {
                ESP_LOGE(TAG, "file too large (> %u bytes)",
                         (unsigned)MAX_INMEM_BYTES);
                ok = false;
                break;
            }
            memcpy(mem + filled, chunk, (size_t)n);
            filled += (size_t)n;
        }

        shutdown(sock, SHUT_RDWR);
        close(sock);
        ESP_LOGI(TAG, "client disconnected (Port 3000)");
		
        if (!ok || filled == 0) {
            ESP_LOGW(TAG, "transfer aborted/empty (total=%u)", (unsigned)filled);
            free(mem);
            continue;
        }

        ESP_LOGI(TAG, "transfer OK: %u bytes. starting playback", (unsigned)filled);

        int pr = audio_manager_play_from_flash(mem, mem + filled);
        
        if (pr == 0) {
            ESP_LOGI(TAG, "playback finished. total=%u bytes%s",
                     (unsigned)filled, header_ok ? "" : " (header not verified)");
        } else {
            ESP_LOGE(TAG, "playback failed (err=%d)", pr);
        }

        free(mem);
    }

    free(chunk);
    close(listen_sock);
    vTaskDelete(NULL);
}

void softap_reverse_audio_start(uint16_t port)
{
    dump_internal_mem(TAG);
    xTaskCreate(reverse_audio, "mp3_server_task", 3072, NULL, 5, &s_mp3_task);
}

static void haptic_pwm_init(void)
{
    if (s_haptic_pwm_inited) {
        return;
    }

    ledc_timer_config_t timer_cfg = {
        .speed_mode      = HAPTIC_PWM_MODE,
        .duty_resolution = HAPTIC_PWM_RES,
        .timer_num       = HAPTIC_PWM_TIMER,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK( ledc_timer_config(&timer_cfg) );

    ledc_channel_config_t ch_l = {
        .gpio_num   = HAPTIC_PWM_GPIO_L,
        .speed_mode = HAPTIC_PWM_MODE,
        .channel    = HAPTIC_PWM_CH_L,
        .timer_sel  = HAPTIC_PWM_TIMER,
        .duty       = 0,
        .hpoint     = 0,
        .intr_type  = LEDC_INTR_DISABLE,
    };
    ESP_ERROR_CHECK( ledc_channel_config(&ch_l) );

    ledc_channel_config_t ch_r = {
        .gpio_num   = HAPTIC_PWM_GPIO_R,
        .speed_mode = HAPTIC_PWM_MODE,
        .channel    = HAPTIC_PWM_CH_R,
        .timer_sel  = HAPTIC_PWM_TIMER,
        .duty       = 0,
        .hpoint     = 0,
        .intr_type  = LEDC_INTR_DISABLE,
    };
    ESP_ERROR_CHECK( ledc_channel_config(&ch_r) );

    s_haptic_pwm_inited = true;
    ESP_LOGI("HAPTIC", "PWM initialized on GPIO L=%d (ch%d), R=%d (ch%d)",
             HAPTIC_PWM_GPIO_L, HAPTIC_PWM_CH_L,
             HAPTIC_PWM_GPIO_R, HAPTIC_PWM_CH_R);
}

static bool parse_six_chars_to_lr(const uint8_t six[6], uint16_t *L, uint16_t *R)
{
    // Parse 6 digits (0-9) into left and right values
    for (int i = 0; i < 6; ++i) {
        if (six[i] < '0' || six[i] > '9') return false;
    }
    int l = (six[0]-'0')*100 + (six[1]-'0')*10 + (six[2]-'0');
    int r = (six[3]-'0')*100 + (six[4]-'0')*10 + (six[5]-'0');
    if (l < 0)   l = 0;
    if (l > 999) l = 999;
    if (r < 0)   r = 0;
    if (r > 999) r = 999;
    *L = (uint16_t)l;
    *R = (uint16_t)r;
    return true;
}

static void vibration_feedback(void *arg)
{
	#if ENABLE_HAPTIC_PWM
	haptic_pwm_init();
	 
	ledc_set_duty(HAPTIC_PWM_MODE, HAPTIC_PWM_CH_L, 0);
    ledc_update_duty(HAPTIC_PWM_MODE, HAPTIC_PWM_CH_L);
    ledc_set_duty(HAPTIC_PWM_MODE, HAPTIC_PWM_CH_R, 0);
    ledc_update_duty(HAPTIC_PWM_MODE, HAPTIC_PWM_CH_R);
	#endif
	
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) { ESP_LOGE(TAG, "ctrl socket() failed"); vTaskDelete(NULL); return; }

    int yes = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(CONTROL_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(listen_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "ctrl bind failed");
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }
    if (listen(listen_sock, 1) < 0) {
        ESP_LOGE(TAG, "ctrl listen failed");
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Control server listening on 0.0.0.0:%d", CONTROL_PORT);

    uint8_t rxbuf[CONTROL_RX_BUFSZ];
    uint8_t window[6];
    size_t  have = 0;

    for (;;)
    {
        struct sockaddr_in cli; socklen_t sl = sizeof(cli);
        int sock = accept(listen_sock, (struct sockaddr*)&cli, &sl);
        if (sock < 0) continue;

        char ip[16]; inet_ntoa_r(cli.sin_addr, ip, sizeof(ip));
        ESP_LOGI(TAG, "CTRL client %s:%d connected", ip, ntohs(cli.sin_port));

        have = 0;

        bool ok = true;
        while (ok) {
            int n = recv(sock, (char*)rxbuf, sizeof(rxbuf), 0);
            if (n == 0) break;
            if (n < 0) { ok = false; break; }

            // Slide a 6-byte window over incoming data, parse in 6-byte chunks
            size_t off = 0;
            while (off < (size_t)n) {
                while (have < 6 && off < (size_t)n) {
                    window[have++] = rxbuf[off++];
                }
				if (have == 6) {
				    uint16_t L, R;
				    if (parse_six_chars_to_lr(window, &L, &R)) {
				        ESP_LOGI(TAG, "Speeds received: L=%u, R=%u", (unsigned)L, (unsigned)R);
		        
					#if ENABLE_HAPTIC_PWM
				        // Map 0-999 to 0-1023 (10-bit duty)
					    uint32_t dutyL = (uint32_t)((L * 1023U + 499U) / 999U);
					    uint32_t dutyR = (uint32_t)((R * 1023U + 499U) / 999U);
				
				        ledc_set_duty(HAPTIC_PWM_MODE, HAPTIC_PWM_CH_L, dutyL);
				        ledc_update_duty(HAPTIC_PWM_MODE, HAPTIC_PWM_CH_L);
				
				        ledc_set_duty(HAPTIC_PWM_MODE, HAPTIC_PWM_CH_R, dutyR);
				        ledc_update_duty(HAPTIC_PWM_MODE, HAPTIC_PWM_CH_R);
				
				        ESP_LOGI(TAG, "PWM duty: L=%"PRIu32"/1023, R=%"PRIu32"/1023", dutyL, dutyR);
				    #endif
				    } else {
				        ESP_LOGW(TAG, "invalid 6-char packet: '%c%c%c%c%c%c'",
				                 window[0], window[1], window[2], window[3], window[4], window[5]);
				    }
				    have = 0;
				}
            }
        }

        shutdown(sock, SHUT_RDWR);
        close(sock);
        ESP_LOGI(TAG, "CTRL client disconnected (Port 4000)");
    }

    close(listen_sock);
    vTaskDelete(NULL);
}

void softap_feedback_start(uint16_t port_unused)
{
    (void)port_unused;
    if (s_vibration_task) {
        ESP_LOGI(TAG, "Control server already running on %d", CONTROL_PORT);
        return;
    }
    dump_internal_mem(TAG);
    xTaskCreate(vibration_feedback, "vibration_feedback", 3072, NULL, 5, &s_vibration_task);
}

static void key_volume_task(void *arg)
{
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);

    audio_board_key_init(set);

    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);
    audio_event_iface_set_listener(esp_periph_set_get_event_iface(set), evt);

    int volume = 50;
    if (audio_manager_get_volume(&volume) != ESP_OK) {
        volume = 50;
    }

    ESP_LOGI(TAG, "key_volume_task started, initial volume=%d%%", volume);

    while (1) {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK) {
            continue;
        }

        // Handle key press events
        if ((msg.source_type == PERIPH_ID_TOUCH ||
             msg.source_type == PERIPH_ID_BUTTON ||
             msg.source_type == PERIPH_ID_ADC_BTN) &&
            (msg.cmd == PERIPH_TOUCH_TAP ||
             msg.cmd == PERIPH_BUTTON_PRESSED ||
             msg.cmd == PERIPH_ADC_BUTTON_PRESSED)) {

            if ((int)msg.data == get_input_volup_id()) {
                volume += 10;
                if (volume > 100) volume = 100;
                audio_manager_set_volume(volume);
                ESP_LOGI(TAG, "Vol+: %d%%", volume);
            } 
            else if ((int)msg.data == get_input_voldown_id()) {
                volume -= 10;
                if (volume < 0) volume = 0;
                audio_manager_set_volume(volume);
                ESP_LOGI(TAG, "Vol-: %d%%", volume);
            }
        }
    }
    vTaskDelete(NULL);
}

void softap_keys_start(void)
{
    if (s_key_task) return;
    dump_internal_mem(TAG);
    xTaskCreate(key_volume_task, "key_volume_task", 3072, NULL, 5, &s_key_task);
}
