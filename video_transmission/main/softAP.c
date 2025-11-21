// ESP32-S3 SoftAP + （可选）MJPEG 推流 + MP3 接收服务器（仅接收，不外放）
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

// ==================== 可按需覆盖的默认配置 ====================
#define WIFI_SSID       "fanghb"
#define WIFI_PASS       "eecs473_15"
#define WIFI_CHANNEL    6
#define MAX_STA_CONN    2
#define VIDEO_PORT 2000  // Video 传输端口
#define REVERSE_AUDIO_PORT      3000   // MP3 接收端口

#define MAX_INMEM_BYTES (1 * 1024 * 1024)   // 2MB
#define RECV_CHUNK (4 * 1024)

#define CONTROL_PORT 4000   // Vibration feedback 接收端口

#define CONTROL_RX_BUFSZ 64 // 小缓冲即可

// === Haptic PWM config ===
#define HAPTIC_PWM_GPIO_L   4      // 左通道：IO4
#define HAPTIC_PWM_GPIO_R   7      // 右通道：IO7

#define HAPTIC_PWM_TIMER    LEDC_TIMER_0
#define HAPTIC_PWM_MODE     LEDC_LOW_SPEED_MODE
#define HAPTIC_PWM_RES      LEDC_TIMER_10_BIT   // 0~1023

#define HAPTIC_PWM_CH_L     LEDC_CHANNEL_0
#define HAPTIC_PWM_CH_R     LEDC_CHANNEL_1

#define ENABLE_PWM   0

static const char *TAG = "softap";

static audio_player_t s_player;

static TaskHandle_t s_video_task = NULL;
static TaskHandle_t s_mp3_task = NULL;
static TaskHandle_t s_vibration_task = NULL;
static TaskHandle_t s_key_task = NULL;

static bool s_haptic_pwm_inited = false;

// ==================== SoftAP 实现 ====================
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

// ==================== MJPEG 推流 ====================
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
        ESP_LOGI(TAG, "client disconnected");
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
    xTaskCreate(video_transmitting, "video_transmitting", 8192, NULL, 5, &s_video_task);
}



// ==================== Reverse_Audio ====================

esp_err_t softap_audio_player_init(void)
{
//    if (audio_player_init(&s_player) == 0) {
//        ESP_LOGI(TAG, "audio player ready (mp3->i2s->ES8311)");
//        return ESP_OK;
//    }
//    ESP_LOGE(TAG, "audio player init failed");
//    return ESP_FAIL;
    ESP_LOGI(TAG, "softap_audio_player_init: no-op (audio_manager owns audio)");
    return ESP_OK;
}


static inline bool looks_like_mp3(const uint8_t *buf, size_t n)
{
    if (n >= 3 && buf[0]=='I' && buf[1]=='D' && buf[2]=='3') return true;            // ID3v2
    if (n >= 2 && buf[0]==0xFF && (buf[1] & 0xE0) == 0xE0) return true;              // 帧同步
    return false;
}

static void reverse_audio(void *arg)
{
    // 监听
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
    ESP_LOGI(TAG, "MP3 mem-play server listening on 0.0.0.0:%d", REVERSE_AUDIO_PORT);

    // 接收缓冲（小块）
    uint8_t *chunk = (uint8_t*)heap_caps_malloc(RECV_CHUNK, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (!chunk) { ESP_LOGE(TAG, "recv chunk alloc failed"); close(listen_sock); vTaskDelete(NULL); return; }

    for (;;)
    {
        struct sockaddr_in cli; socklen_t sl = sizeof(cli);
        int sock = accept(listen_sock, (struct sockaddr*)&cli, &sl);
        if (sock < 0) continue;

        char ip[16]; inet_ntoa_r(cli.sin_addr, ip, sizeof(ip));
        ESP_LOGI(TAG, "Client %s:%d connected (A-only: in-mem MP3)", ip, ntohs(cli.sin_port));

        // 大缓冲：整段 MP3 收到内存
        uint8_t *mem = (uint8_t*)heap_caps_malloc(MAX_INMEM_BYTES, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
        if (!mem) {
            ESP_LOGE(TAG, "big buffer alloc failed (need PSRAM). Lower MAX_INMEM_BYTES or enable PSRAM.");
            shutdown(sock, SHUT_RDWR); close(sock);
            continue;
        }

        size_t filled = 0;
        bool header_checked = false, header_ok = false, ok = true;

        while (1) {
            int n = recv(sock, (char*)chunk, RECV_CHUNK, 0);
            if (n == 0) break;              // 正常断开
            if (n < 0) { ok = false; break; }

            if (!header_checked) {
                header_ok = looks_like_mp3(chunk, (size_t)n);
                header_checked = true;
                if (!header_ok) {
                    ESP_LOGW(TAG, "incoming data doesn't look like MP3 (no ID3/sync)");
                }
            }

            if (filled + (size_t)n > MAX_INMEM_BYTES) {
                ESP_LOGE(TAG, "file too large for in-memory playback (> %u bytes).",
                         (unsigned)MAX_INMEM_BYTES);
                ok = false;
                break;
            }
            memcpy(mem + filled, chunk, (size_t)n);
            filled += (size_t)n;
        }

        shutdown(sock, SHUT_RDWR);
        close(sock);

        if (!ok || filled == 0) {
            ESP_LOGW(TAG, "transfer aborted/empty (total=%u)", (unsigned)filled);
            free(mem);
            continue;
        }

        ESP_LOGI(TAG, "Transfer OK: %u bytes. Start playback (in-mem).", (unsigned)filled);

        //int pr = audio_player_play_from_flash(&s_player, mem, mem + filled);
		int pr = audio_manager_play_from_flash(mem, mem + filled);
        
        if (pr == 0) {
            ESP_LOGI(TAG, "Playback finished. total=%u bytes%s",
                     (unsigned)filled, header_ok ? "" : " (header not verified)");
        } else {
            ESP_LOGE(TAG, "Playback failed (err=%d)", pr);
        }

        free(mem);
    }

    free(chunk);
    close(listen_sock);
    vTaskDelete(NULL);
}

void softap_reverse_audio_start(uint16_t port)
{
    (void)port; // 端口仍用 AUDIO_PORT 宏，保持兼容
    if (s_mp3_task) {
        ESP_LOGI(TAG, "MP3 server already running on port %d", REVERSE_AUDIO_PORT);
        return;
    }
    xTaskCreate(reverse_audio, "mp3_server_task", 4096, NULL, 5, &s_mp3_task);
}

// ==================== PWM_Initialization ====================

static void haptic_pwm_init(void)
{
    if (s_haptic_pwm_inited) {
        return;
    }

    // 1) 配置 LEDC 定时器
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = HAPTIC_PWM_MODE,
        .duty_resolution = HAPTIC_PWM_RES,
        .timer_num       = HAPTIC_PWM_TIMER,
        .freq_hz         = 5000,          // 5kHz，根据你马达需要可以改
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK( ledc_timer_config(&timer_cfg) );

    // 2) 配置左通道 (L -> IO4)
    ledc_channel_config_t ch_l = {
        .gpio_num   = HAPTIC_PWM_GPIO_L,
        .speed_mode = HAPTIC_PWM_MODE,
        .channel    = HAPTIC_PWM_CH_L,
        .timer_sel  = HAPTIC_PWM_TIMER,
        .duty       = 0,                 // 初始 0
        .hpoint     = 0,
        .intr_type  = LEDC_INTR_DISABLE,
    };
    ESP_ERROR_CHECK( ledc_channel_config(&ch_l) );

    // 3) 配置右通道 (R -> IO7)
    ledc_channel_config_t ch_r = {
        .gpio_num   = HAPTIC_PWM_GPIO_R,
        .speed_mode = HAPTIC_PWM_MODE,
        .channel    = HAPTIC_PWM_CH_R,
        .timer_sel  = HAPTIC_PWM_TIMER,
        .duty       = 0,                 // 初始 0
        .hpoint     = 0,
        .intr_type  = LEDC_INTR_DISABLE,
    };
    ESP_ERROR_CHECK( ledc_channel_config(&ch_r) );

    s_haptic_pwm_inited = true;
    ESP_LOGI("HAPTIC", "PWM inited on GPIO L=%d (ch%d), R=%d (ch%d)",
             HAPTIC_PWM_GPIO_L, HAPTIC_PWM_CH_L,
             HAPTIC_PWM_GPIO_R, HAPTIC_PWM_CH_R);
}


// ==================== Vibration_Feedback ====================

static bool parse_six_chars_to_lr(const uint8_t six[6], uint16_t *L, uint16_t *R)
{
    // 只能是 '0'..'9'
    for (int i = 0; i < 6; ++i) {
        if (six[i] < '0' || six[i] > '9') return false;
    }
    int l = (six[0]-'0')*100 + (six[1]-'0')*10 + (six[2]-'0');
    int r = (six[3]-'0')*100 + (six[4]-'0')*10 + (six[5]-'0');
    if (l < 0 || l > 100 || r < 0 || r > 100) return false;
    *L = (uint16_t)l;
    *R = (uint16_t)r;
    return true;
}

static void vibration_feedback(void *arg)
{
	
	#if ENABLE_HAPTIC_PWM
	 haptic_pwm_init();
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

        // 清空窗口
        have = 0;

        // 可选：设置接收超时
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        bool ok = true;
        while (ok) {
            int n = recv(sock, (char*)rxbuf, sizeof(rxbuf), 0);
            if (n == 0) break;           // 正常断开
            if (n < 0) { ok = false; break; }

            // 把收到的数据流入一个 6 字节窗口，按 6 的步长解析
            size_t off = 0;
            while (off < (size_t)n) {
                // 填满窗口
                while (have < 6 && off < (size_t)n) {
                    window[have++] = rxbuf[off++];
                }
				if (have == 6) {
				    uint16_t L, R;
				    if (parse_six_chars_to_lr(window, &L, &R)) {
				        // 1) 打印原始 0~100 scale 的值
				        ESP_LOGI(TAG, "Speeds received: L=%u, R=%u", (unsigned)L, (unsigned)R);
				
				        // 2) clamp 到 0~100，避免异常输入
				        if (L > 100) L = 100;
				        if (R > 100) R = 100;
						#if ENABLE_HAPTIC_PWM
				        // 3) 放缩到 0~1023 (10-bit duty)
				        //    简单线性映射: duty = round( L/100 * 1023 )
				        uint32_t dutyL = (uint32_t)((L * 1023 + 50) / 100);  // +50 做四舍五入
				        uint32_t dutyR = (uint32_t)((R * 1023 + 50) / 100);
				
				        // 4) 更新 PWM 输出
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
				    have = 0; // 准备下一帧
				}
            }
        }

        shutdown(sock, SHUT_RDWR);
        close(sock);
        ESP_LOGI(TAG, "CTRL client disconnected");
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
    xTaskCreate(vibration_feedback, "vibration_feedback", 4096, NULL, 5, &s_vibration_task);
}

// ==================== Volume_Adjustment ====================

static void key_volume_task(void *arg)
{
    // 1. 建立 periph_set
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);

    // 2. 初始化板载按键（根据 board 配置映射到 TOUCH/GPIO/ADC）
    audio_board_key_init(set);

    // 3. 创建事件接口 & 绑定 periph_set
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

        // 只处理按键事件
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
    xTaskCreate(key_volume_task, "key_volume_task", 4096, NULL, 5, &s_key_task);
}
