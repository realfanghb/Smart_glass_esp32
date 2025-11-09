// ESP32-S3 SoftAP + （可选）MJPEG 推流 + MP3 接收服务器（仅接收，不外放）
#include <string.h>
#include <inttypes.h>

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

#include "softAP.h"
#include "audio_play.h"

// ==================== 可按需覆盖的默认配置 ====================
#ifndef WIFI_SSID
#define WIFI_SSID       "fanghb"
#endif
#ifndef WIFI_PASS
#define WIFI_PASS       "eecs473_15"
#endif
#ifndef WIFI_CHANNEL
#define WIFI_CHANNEL    6
#endif
#ifndef MAX_STA_CONN
#define MAX_STA_CONN    2
#endif
#ifndef VIDEO_PORT
#define VIDEO_PORT 2000  // Video 传输端口
#endif

#ifndef REVERSE_AUDIO_PORT
#define REVERSE_AUDIO_PORT      3000   // MP3 接收端口
#endif

#ifndef MAX_INMEM_BYTES
#define MAX_INMEM_BYTES (1 * 1024 * 1024)   // 2MB
#endif

#ifndef RECV_CHUNK
#define RECV_CHUNK (4 * 1024)
#endif

#ifndef CONTROL_PORT
#define CONTROL_PORT 4000   // Vibration feedback 接收端口
#endif

#ifndef CONTROL_RX_BUFSZ
#define CONTROL_RX_BUFSZ 64 // 小缓冲即可
#endif


static const char *TAG = "softap";

static audio_player_t s_player;

static TaskHandle_t s_video_task = NULL;
static TaskHandle_t s_mp3_task = NULL;
static TaskHandle_t s_vibration_task = NULL;

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
    if (audio_player_init(&s_player) == 0) {
        ESP_LOGI(TAG, "audio player ready (mp3->i2s->ES8311)");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "audio player init failed");
    return ESP_FAIL;
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

        // 直接从内存播放
        int pr = audio_player_play_from_flash(&s_player, mem, mem + filled);
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
						ESP_LOGI(TAG, "Speeds received: L=%u, R=%u", (unsigned)L, (unsigned)R);                    
					} 
					else {
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
