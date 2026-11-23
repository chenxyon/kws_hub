/**
 * @file     kws_hub.cpp
 * @brief    唤醒词检测中枢：引擎调度 + 唤醒词匹配
 *
 * 职责边界：
 *   - 本文件只管「选谁干活 / 拿到文字后判断是不是唤醒 / 事件分发」
 *   - 具体识别由引擎完成（讯飞在线听写 / ESP-SR 离线）
 *
 * 修改：2026-11-21 重构为 KwsHub 静态类；
 *       修复引擎初始化失败置空引擎指针而监督任务仍在跑的空指针崩溃
 */
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "kws_hub.h"
#include "kws_config.h"
#include "kws_xfyun.h"
#include "kws_esp_sr.h"

static const char *TAG = "KWS_HUB";

/* ==================== 静态成员初始化 ==================== */

const KwsEngine *KwsHub::s_engine = nullptr;
KwsEventCb KwsHub::s_cb = nullptr;
void *KwsHub::s_cbCtx = nullptr;
char KwsHub::s_wake[KwsHub::TEXT_MAX] = {0};
char KwsHub::s_alias[KwsHub::MAX_ALIAS][KwsHub::TEXT_MAX] = {{0}};
int KwsHub::s_aliasNum = 0;
volatile bool KwsHub::s_listening = false;
volatile bool KwsHub::s_paused = false;
bool KwsHub::s_supCreated = false;
bool KwsHub::s_wakeFired = false;
volatile int64_t KwsHub::s_wakeUs = 0;

/* ==================== 唤醒词归一化与匹配 ==================== */

/* 去掉空格与常见标点，让「小智， 小智」也能命中「小智小智」。
 * 中文按 UTF-8 原样保留，只删 ASCII 空白与标点。 */
void KwsHub::normalize(const char *in, char *out, size_t outN)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < outN; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
            c == ',' || c == '.' || c == '?' || c == '!' ||
            c == '~' || c == '-' || c == '_' || c == ':' || c == ';') {
            continue;
        }
        out[o++] = (char)c;
    }
    out[o] = '\0';
}

/* 命中则返回命中的那个词，否则 nullptr */
const char *KwsHub::matchWake(const char *normText)
{
    const char *hit = nullptr;
    if (s_wake[0] && strstr(normText, s_wake)) hit = s_wake;
    for (int i = 0; i < s_aliasNum && !hit; i++) {
        if (s_alias[i][0] && strstr(normText, s_alias[i])) hit = s_alias[i];
    }
    return hit;
}

/* ==================== 引擎上报入口 ==================== */

void KwsHub::emitText(const char *text, bool isFinal)
{
    if (!text || !text[0]) return;

    if (s_wakeFired) {
        /* 已唤醒：这段文字当指令上报 */
        ESP_LOGI(TAG, "指令文本: %s", text);
        if (s_cb) s_cb(KwsEvent::TEXT, text, s_cbCtx);
        /* 一次唤醒只取第一句指令，之后回到等待唤醒 */
        s_wakeFired = false;
        return;
    }

    if (!isFinal) return;   /* 只记录整句结果，避免中间态刷屏 */
    ESP_LOGI(TAG, "识别文本: %s", text);

    char norm[TEXT_MAX];
    normalize(text, norm, sizeof(norm));
    const char *hit = matchWake(norm);
    if (!hit) return;

    ESP_LOGI(TAG, "命中唤醒词 [%s]：%s", hit, text);
    s_wakeFired = true;
    s_wakeUs = esp_timer_get_time();
    if (s_cb) s_cb(KwsEvent::WAKE, text, s_cbCtx);
}

void KwsHub::emitError(const char *msg)
{
    ESP_LOGE(TAG, "引擎错误: %s", msg ? msg : "?");
    if (s_cb) s_cb(KwsEvent::ERROR, msg, s_cbCtx);
}

/* ==================== 监督任务 ==================== */

/* 常驻任务（init 时创建一次，永不退出）。
 *
 * 设计要点：start/stop 只翻标志位、立即返回，绝不阻塞调用者。
 * 真正的引擎建连/收尾（关 TLS 可能要数秒）全在这里做 ——
 * 因为 stop() 可能从 WiFi 事件任务里调用，绝不能让事件循环干等。 */
void KwsHub::supervisorTask(void *arg)
{
    (void)arg;
    for (;;) {
        /* 防御：引擎未就绪时只休眠（修复原实现初始化失败置空后的空指针崩溃） */
        if (s_engine == nullptr) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        /* 唤醒后迟迟没提问 → 回到唤醒词监听，避免卡在"等你问"导致后面喊小智没反应 */
        if (s_wakeFired && (esp_timer_get_time() - s_wakeUs) > 20 * 1000000LL) {
            s_wakeFired = false;
            ESP_LOGI(TAG, "等待提问超时(20s)，恢复唤醒词监听");
        }

        /* 不该监听（已停/暂停）时，确保会话已关闭 */
        if (!s_listening || s_paused) {
            if (s_engine->busy()) s_engine->stop();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (s_engine->start() != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        while (s_listening && !s_paused && s_engine->busy()) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!s_listening || s_paused) {
            s_engine->stop();       /* 主动收尾，释放 TLS/websocket 内存 */
        } else {
            vTaskDelay(pdMS_TO_TICKS(300));  /* 避免频繁重连打爆服务端 */
        }
    }
}

/* ==================== 对外接口 ==================== */

esp_err_t KwsHub::init(KwsEngineId engine)
{
    KwsEngineId id = engine;
    if (id == KwsEngineId::NONE) {
        id = KWS_ACTIVE_ENGINE;   /* kws_config.h 默认 */
    }
    const KwsEngine *eng = nullptr;
    switch (id) {
    case KwsEngineId::XFYUN_IAT: eng = &KwsXfyun::engine(); break;
    case KwsEngineId::ESP_SR:    eng = &KwsEspSr::engine(); break;
    default:
        ESP_LOGE(TAG, "未知唤醒引擎 id=%d", (int)id);
        return ESP_ERR_INVALID_ARG;
    }

    /* 默认唤醒词取自配置，运行时可再改 */
    if (!s_wake[0]) {
        const char *def = CONFIG_KWS_WAKE_WORD;
        strncpy(s_wake, def[0] ? def : "xiaozhi", sizeof(s_wake) - 1);
        s_wake[sizeof(s_wake) - 1] = '\0';
    }
    normalize(s_wake, s_wake, sizeof(s_wake));   /* 归一化后存回 */

    esp_err_t err = eng->init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "引擎 %s 初始化失败: %s", eng->name, esp_err_to_name(err));
        /* 修复：不置空已有引擎指针，避免监督任务空指针；返回错误由调用方决策 */
        return err;
    }
    s_engine = eng;

    /* 监督任务常驻：只创建一次（init 可能因 WiFi 重连被重复调用） */
    if (!s_supCreated) {
        if (xTaskCreate(supervisorTask, "kws_sup", 4096, nullptr, 4, nullptr) != pdPASS) {
            ESP_LOGE(TAG, "监督任务创建失败");
            return ESP_ERR_NO_MEM;
        }
        s_supCreated = true;
    }
    ESP_LOGI(TAG, "初始化完成，引擎=%s，唤醒词=%s", s_engine->name, s_wake);
    return ESP_OK;
}

esp_err_t KwsHub::setWakeWord(const char *word)
{
    if (!word || !word[0]) return ESP_ERR_INVALID_ARG;
    char norm[TEXT_MAX];
    normalize(word, norm, sizeof(norm));
    if (!norm[0]) return ESP_ERR_INVALID_ARG;
    strncpy(s_wake, norm, sizeof(s_wake) - 1);
    s_wake[sizeof(s_wake) - 1] = '\0';
    ESP_LOGI(TAG, "唤醒词设为: %s", s_wake);
    return ESP_OK;
}

esp_err_t KwsHub::addWakeAlias(const char *word)
{
    if (!word || !word[0]) return ESP_ERR_INVALID_ARG;
    if (s_aliasNum >= MAX_ALIAS) return ESP_ERR_NO_MEM;
    char norm[TEXT_MAX];
    normalize(word, norm, sizeof(norm));
    if (!norm[0]) return ESP_ERR_INVALID_ARG;
    strncpy(s_alias[s_aliasNum], norm, sizeof(s_alias[0]) - 1);
    s_alias[s_aliasNum][sizeof(s_alias[0]) - 1] = '\0';
    ESP_LOGI(TAG, "别名[%d] += %s", s_aliasNum, s_alias[s_aliasNum]);
    s_aliasNum++;
    return ESP_OK;
}

const char *KwsHub::wakeWord()
{
    return s_wake;
}

esp_err_t KwsHub::setCallback(KwsEventCb cb, void *ctx)
{
    s_cb = cb;
    s_cbCtx = ctx;
    return ESP_OK;
}

esp_err_t KwsHub::start()
{
    if (!s_engine) return ESP_ERR_INVALID_STATE;
    s_wakeFired = false;
    s_paused = false;
    s_listening = true;      /* 监督任务会自动建会话，本函数不阻塞 */
    ESP_LOGI(TAG, "开始监听唤醒词");
    return ESP_OK;
}

esp_err_t KwsHub::stop()
{
    /* 只置标志，立即返回；会话收尾由监督任务异步完成。
     * 这样从 WiFi 事件回调里调用也不会阻塞事件循环。 */
    if (s_listening) ESP_LOGI(TAG, "停止监听唤醒词");
    s_listening = false;
    return ESP_OK;
}

esp_err_t KwsHub::pause()
{
    if (!s_listening || s_paused) return ESP_OK;
    s_paused = true;
    ESP_LOGI(TAG, "暂停监听（TTS 播报中）");
    return ESP_OK;
}

esp_err_t KwsHub::resume()
{
    if (!s_paused) return ESP_OK;
    s_paused = false;
    ESP_LOGI(TAG, "恢复监听");
    return ESP_OK;
}

bool KwsHub::isListening()
{
    return s_listening;
}

const char *KwsHub::engineName()
{
    return s_engine ? s_engine->name : "none";
}

#include "kws_hub_version.h"
#include "version_manager.h"
COMPONENT_VERSION_REGISTER(kws_hub, KWS_HUB_VERSION_STRING)
