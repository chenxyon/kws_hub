/**
 * @file     kws_xfyun.cpp
 * @brief    讯飞在线听写唤醒引擎实现
 *
 * 修改：2026-11-21 重构为 KwsXfyun 类；鉴权改经 XfyunAuth；
 *       新增运行时凭据注入；鉴权 URL 大缓冲挪静态区
 */
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/base64.h"
#include "cJSON.h"

#include "inmp441_bridge.h"
#include "xfyun_auth.h"
#include "kws_config.h"

#include "kws_xfyun.h"

static const char *TAG = "KWS_XFYUN";

#define XF_HOST  "iat-api.xfyun.cn"
#define XF_PATH  "/v2/iat"

/* 讯飞听写推荐：每 40ms 发 1280 字节（=640 采样@16kHz，正好实时）*/
#define IAT_CHUNK_BYTES   1280
#define IAT_CHUNK_MS      40
#define IAT_RX_BUF        4096   /* v2 返回报文较小，够用 */
#define IAT_TEXT_MAX      512    /* 整句累积缓冲 */

/* 引擎会话状态 */
static volatile bool s_running;
static volatile bool s_session_busy;
static volatile bool s_task_alive;
static volatile bool s_ws_error;
static esp_websocket_client_handle_t s_cli;
static char s_rx[IAT_RX_BUF];
static size_t s_rxlen;
static int s_seq;
static char s_text[IAT_TEXT_MAX];   /* 跨分片累积整句 */

/* ==================== 凭据（运行时注入 → IAT 宏 → TTS 宏） ==================== */

char KwsXfyun::s_appId[32] = {0};
char KwsXfyun::s_apiKey[64] = {0};
char KwsXfyun::s_apiSecret[64] = {0};
bool KwsXfyun::s_credsSet = false;

void KwsXfyun::setCredentials(const char *appId, const char *apiKey, const char *apiSecret)
{
    if (appId == nullptr || apiKey == nullptr || apiSecret == nullptr) return;
    strncpy(s_appId, appId, sizeof(s_appId) - 1);
    s_appId[sizeof(s_appId) - 1] = '\0';
    strncpy(s_apiKey, apiKey, sizeof(s_apiKey) - 1);
    s_apiKey[sizeof(s_apiKey) - 1] = '\0';
    strncpy(s_apiSecret, apiSecret, sizeof(s_apiSecret) - 1);
    s_apiSecret[sizeof(s_apiSecret) - 1] = '\0';
    s_credsSet = true;
}

const char *KwsXfyun::effAppId()
{
    if (s_credsSet && s_appId[0]) return s_appId;
    return (ASR_XFYUN_APPID[0] != '\0') ? ASR_XFYUN_APPID : TTS_XFYUN_APPID;
}

const char *KwsXfyun::effApiKey()
{
    if (s_credsSet && s_apiKey[0]) return s_apiKey;
    return (ASR_XFYUN_API_KEY[0] != '\0') ? ASR_XFYUN_API_KEY : TTS_XFYUN_API_KEY;
}

const char *KwsXfyun::effApiSecret()
{
    if (s_credsSet && s_apiSecret[0]) return s_apiSecret;
    return (ASR_XFYUN_API_SECRET[0] != '\0') ? ASR_XFYUN_API_SECRET
                                              : TTS_XFYUN_API_SECRET;
}

/* ==================== 发送 ==================== */

/* v2 音频格式串：16k 16bit 小端单声道 */
#define IAT_AUDIO_FORMAT "audio/L16;rate=16000"

/* 发一帧音频。status: 0=首帧(带参数) 1=中间 2=结束 */
static void sendAudioFrame(const uint8_t *pcm, size_t len, int status)
{
    static char b64[IAT_CHUNK_BYTES / 3 * 4 + 8];
    char *js;
    size_t olen = 0;

    b64[0] = '\0';
    if (len) {
        if (mbedtls_base64_encode((unsigned char *)b64, sizeof(b64), &olen,
                                  pcm, len) != 0) {
            return;
        }
        b64[olen] = '\0';
    }

    if (status == 0) {
        /* 首帧：common + business + data */
        js = (char *)malloc(1024 + olen);
        if (!js) return;
        snprintf(js, 1024 + olen,
                 "{\"common\":{\"app_id\":\"%s\"},"
                 "\"business\":{\"language\":\"zh_cn\",\"domain\":\"%s\","
                 "\"accent\":\"mandarin\",\"vad_eos\":%d},"
                 "\"data\":{\"status\":0,\"format\":\"" IAT_AUDIO_FORMAT "\","
                 "\"encoding\":\"raw\",\"audio\":\"%s\"}}",
                 KwsXfyun::effAppId(), CONFIG_KWS_IAT_DOMAIN,
                 CONFIG_KWS_IAT_EOS_MS, b64);
    } else {
        js = (char *)malloc(384 + olen);
        if (!js) return;
        snprintf(js, 384 + olen,
                 "{\"data\":{\"status\":%d,\"format\":\"" IAT_AUDIO_FORMAT "\","
                 "\"encoding\":\"raw\",\"audio\":\"%s\"}}",
                 status, b64);
    }

    esp_websocket_client_send_text(s_cli, js, strlen(js), pdMS_TO_TICKS(200));
    free(js);
    s_seq++;
}

/* ==================== 接收解析 ==================== */

/* 把一段 result.ws[].cw[].w 拼成字符串，追加到 s_text */
static void appendWords(cJSON *result)
{
    cJSON *ws = cJSON_GetObjectItem(result, "ws");
    int nseg = cJSON_GetArraySize(ws);
    for (int s = 0; s < nseg; s++) {
        cJSON *seg = cJSON_GetArrayItem(ws, s);
        cJSON *cw = cJSON_GetObjectItem(seg, "cw");
        int ncw = cJSON_GetArraySize(cw);
        for (int i = 0; i < ncw; i++) {
            cJSON *w = cJSON_GetObjectItem(cJSON_GetArrayItem(cw, i), "w");
            if (w && w->valuestring) {
                size_t cur = strlen(s_text);
                size_t l = strlen(w->valuestring);
                if (cur + l < sizeof(s_text)) {
                    memcpy(s_text + cur, w->valuestring, l);
                    s_text[cur + l] = '\0';
                }
            }
        }
    }
}

static void handleResponse(const char *json, size_t len)
{
    cJSON *r = cJSON_ParseWithLength(json, len);
    if (!r) return;

    /* v2 协议：code/message 在顶层，识别结果在 data.result，结束帧 data.status==2 */
    cJSON *code = cJSON_GetObjectItem(r, "code");
    int c = code ? code->valueint : 0;
    if (c != 0) {
        cJSON *msg = cJSON_GetObjectItem(r, "message");
        char buf[128];
        snprintf(buf, sizeof(buf), "code=%d msg=%s", c,
                 (msg && msg->valuestring) ? msg->valuestring : "(无)");
        KwsHub::emitError(buf);
        s_session_busy = false;
        cJSON_Delete(r);
        return;
    }

    cJSON *data = cJSON_GetObjectItem(r, "data");
    if (data) {
        cJSON *res = cJSON_GetObjectItem(data, "result");
        bool final = false;
        if (res) {
            appendWords(res);
            cJSON *ls = cJSON_GetObjectItem(res, "ls");
            final = ls && ls->valueint;
        }
        if (final && s_text[0]) {
            ESP_LOGI(TAG, "识别整句: %s", s_text);
            KwsHub::emitText(s_text, true);
            s_text[0] = '\0';
        }
        cJSON *st = cJSON_GetObjectItem(data, "status");
        if (st && st->valueint == 2) s_session_busy = false;
    }

    cJSON_Delete(r);
}

static void wsEvent(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)data;
    (void)arg; (void)base;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "听写已连接，等待音频");
        s_session_busy = true;
        break;

    case WEBSOCKET_EVENT_DATA: {
        if (s_rxlen + ev->data_len >= sizeof(s_rx)) {
            ESP_LOGW(TAG, "听写返回报文超长(%u)，丢弃", (unsigned)ev->data_len);
            s_rxlen = 0;
            break;
        }
        memcpy(s_rx + s_rxlen, ev->data_ptr, ev->data_len);
        s_rxlen += ev->data_len;

        /* 能解析成完整 JSON 才处理，否则等后续分片。
         * 注意：这里只用于"判断是否收全"，解析出来的树必须立刻释放，
         * 否则每收一条完整返回就泄漏一棵 cJSON 树（内部 RAM 持续下降，
         * 最终 esp-aes/TLS 申请不到内存 → 听写断开重连 → 唤醒失灵）。 */
        cJSON *chk = cJSON_ParseWithLength(s_rx, s_rxlen);
        if (chk != nullptr) {
            cJSON_Delete(chk);
            handleResponse(s_rx, s_rxlen);
            s_rxlen = 0;
        }
        break;
    }

    case WEBSOCKET_EVENT_ERROR:
    case WEBSOCKET_EVENT_CLOSED:
    case WEBSOCKET_EVENT_DISCONNECTED:
        s_session_busy = false;
        s_ws_error = true;
        break;

    default:
        break;
    }
}

/* ==================== 会话任务 ==================== */

static void sessionTask(void *arg)
{
    (void)arg;
    /* 优化：2.6KB 鉴权 URL 放静态区，降低 8KB 任务栈压力
     * （单会话引擎，s_task_alive 保证同时只有一条会话） */
    static char url[2600];

    XfyunAuth auth;
    auth.setCredentials({ KwsXfyun::effApiKey(), KwsXfyun::effApiSecret() });

    if (XfyunAuth::waitSntp(20) != ESP_OK) {
        KwsHub::emitError("SNTP 未同步，鉴权会失败");
        goto finish;
    }
    if (auth.buildAuthUrl(XF_HOST, XF_PATH, url, sizeof(url)) != ESP_OK) {
        KwsHub::emitError("鉴权 URL 生成失败");
        goto finish;
    }

    s_seq  = 1;
    s_rxlen = 0;
    s_text[0] = '\0';
    s_ws_error = false;

    ESP_LOGI(TAG, "建连前内存: 总剩=%u 最大连续块=%u | 内部RAM剩=%u 内部最大块=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    {
        esp_websocket_client_config_t cfg = {
            .uri = url,
            .buffer_size = 2048,
            .task_stack = 4096,
            .disable_auto_reconnect = true,
            .network_timeout_ms = 10000,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        s_cli = esp_websocket_client_init(&cfg);
    }
    if (!s_cli) {
        KwsHub::emitError("websocket 客户端创建失败");
        goto finish;
    }
    esp_websocket_register_events(s_cli, WEBSOCKET_EVENT_ANY, wsEvent, nullptr);
    esp_websocket_client_start(s_cli);

    {
        static int16_t pcm[IAT_CHUNK_BYTES / 2];
        bool connected = false;
        bool ended = false;

        for (int i = 0; i < 100 && s_running; i++) {
            if (s_session_busy) { connected = true; break; }
            if (s_ws_error) break;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!connected) {
            ESP_LOGW(TAG, "听写连接失败/超时");
            goto finish;
        }

        ESP_LOGI(TAG, "开始流式识别（%u ms/%u 字节）", (unsigned)IAT_CHUNK_MS,
                 (unsigned)IAT_CHUNK_BYTES);
        sendAudioFrame(nullptr, 0, 0);

        int64_t deadline = esp_timer_get_time() + 55LL * 1000 * 1000;
        int guard = 0;

        while (s_running && !ended) {
            if (!s_session_busy) { ended = true; break; }
            if (esp_timer_get_time() > deadline) { ended = true; break; }

            size_t got = 0;
            esp_err_t err = inmp441_read(pcm, sizeof(pcm), &got, pdMS_TO_TICKS(300));
            if (err == ESP_OK && got > 0) {
                sendAudioFrame((const uint8_t *)pcm, got, 1);
                guard = 0;   /* 成功即清零，避免偶发超时累积提前结束 */
            } else if (++guard > 20) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(IAT_CHUNK_MS));
        }

        if (s_running) {
            sendAudioFrame(nullptr, 0, 2);
            for (int i = 0; i < 15 && s_session_busy; i++) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
    }

finish:
    s_session_busy = false;
    if (s_cli) {
        esp_websocket_client_stop(s_cli);
        esp_websocket_client_destroy(s_cli);
        s_cli = nullptr;
    }
    s_rxlen = 0;
    s_task_alive = false;
    vTaskDelete(nullptr);
}

/* ==================== 引擎接口 ==================== */

esp_err_t KwsXfyun::init()
{
    if (!inmp441_is_running()) {
        ESP_LOGE(TAG, "麦克风未就绪，唤醒引擎无法工作");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "引擎就绪: %s (v2 听写)", effAppId());
    return ESP_OK;
}

esp_err_t KwsXfyun::start()
{
    /* 关键：以「会话任务是否还活着」判断是否需要重连，而不是 s_running。
     * 之前用 s_running：session_task 正常结束后 s_running 仍是 true，导致
     * 监督任务再调 start() 时不会新建任务 → 第一段会话结束后引擎假死，
     * 后续「小智」完全不会被听。 */
    if (s_task_alive) {
        return ESP_OK;
    }
    s_running = true;
    s_task_alive = true;
    if (xTaskCreate(sessionTask, "kws_iat", 8192, nullptr, 5, nullptr) != pdPASS) {
        s_running = false;
        s_task_alive = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t KwsXfyun::stop()
{
    s_running = false;
    for (int i = 0; i < 100 && s_task_alive; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_task_alive) {
        ESP_LOGW(TAG, "会话任务 5s 未退出，强制置空闲");
    }
    s_session_busy = false;
    return ESP_OK;
}

bool KwsXfyun::busy()
{
    /* 用「任务存活」表示忙：只要会话任务还没结束，监督任务就等它；
     * 任务一结束(busy=false)就立刻重连，保证持续监听不中断。 */
    return s_task_alive;
}

const KwsEngine &KwsXfyun::engine()
{
    static const KwsEngine eng = {
        .name  = "xfyun-iat",
        .init  = KwsXfyun::init,
        .start = KwsXfyun::start,
        .stop  = KwsXfyun::stop,
        .busy  = KwsXfyun::busy,
    };
    return eng;
}
