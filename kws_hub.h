/**
 * @file     kws_hub.h
 * @brief    唤醒词检测组件（引擎可插拔）— C++ class
 *
 * 功能：常驻监听麦克风，把音频喂给「唤醒引擎」；引擎产出识别文字后，
 *       与用户自定义的唤醒词做匹配，命中即回调 KwsEvent::WAKE。
 *       唤醒之后引擎继续识别，后续文字以 KwsEvent::TEXT 上报，可直接当指令用。
 *
 * 引擎说明：
 *   - KwsEngineId::XFYUN_IAT：讯飞在线听写 + 文本匹配。
 *     唤醒词完全自定义（改字符串即可），零模型文件零训练；
 *     代价是依赖网络，唤醒延迟约 1~2 秒。
 *   - KwsEngineId::ESP_SR：ESP-SR WakeNet 离线唤醒（接口已预留）。
 *     延迟 <100ms 且不耗流量；自定义唤醒词需云端训练模型。
 *
 * 音频来源是 Inmp441Mic 组件的环形缓冲，采样率须为 16kHz
 * （讯飞听写与 ESP-SR 唤醒的标准采样率）。
 *
 * 修改：2026-11-21 由 C API（kws_hub_* 函数）重构为 KwsHub 静态类；
 *       修复引擎初始化失败置空引擎指针而监督任务仍在跑导致的空指针崩溃；
 *       会话任务鉴权 URL 大缓冲挪静态区（降低栈压力）
 */
#ifndef KWS_HUB_H
#define KWS_HUB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** 唤醒引擎标识 */
enum class KwsEngineId : uint8_t {
    NONE = 0,
    XFYUN_IAT,  /*!< 讯飞在线听写 + 文本匹配 */
    ESP_SR,     /*!< ESP-SR WakeNet 离线唤醒（预留） */
};

/** 唤醒事件类型 */
enum class KwsEvent : uint8_t {
    WAKE = 0, /*!< 命中唤醒词，text 为识别到的原文 */
    TEXT,     /*!< 识别到文字；唤醒后即为指令文本 */
    ERROR,    /*!< 引擎报错，text 为错误描述 */
};

/**
 * 唤醒事件回调
 *
 * @param ev   事件类型
 * @param text 识别文本（UTF-8，ERROR 时为错误描述，可能为 nullptr）
 * @param ctx  注册时传入的上下文
 */
using KwsEventCb = void (*)(KwsEvent ev, const char *text, void *ctx);

/**
 * 引擎统一形态：引擎自行管理会话与音频读取（从 Inmp441Mic 环形缓冲取），
 * KwsHub 只负责生命周期与事件分发。
 */
struct KwsEngine {
    const char *name = nullptr;          /*!< 引擎名，诊断用 */
    esp_err_t (*init)(void) = nullptr;   /*!< 初始化（麦克风/网络/模型） */
    esp_err_t (*start)(void) = nullptr;  /*!< 开始一个识别会话 */
    esp_err_t (*stop)(void) = nullptr;   /*!< 结束当前会话 */
    bool (*busy)(void) = nullptr;        /*!< 是否正在识别 */
};

/**
 * 唤醒词检测中枢静态类
 *
 * 用法：
 *   KwsHub::setCallback(onKws, nullptr);
 *   KwsHub::init();                       // 引擎按 kws_config.h 默认
 *   KwsHub::setWakeWord("小智小智");
 *   KwsHub::start();
 *   // TTS 播报期间：KwsHub::pause(); 播完：KwsHub::resume();
 */
class KwsHub {
public:
    static constexpr int MAX_ALIAS = 4;   /*!< 主唤醒词之外最多几个别名 */
    static constexpr int TEXT_MAX = 256;  /*!< 唤醒词/识别文本缓冲 */

    /**
     * @brief 初始化组件并选定引擎
     *
     * @param engine 引擎标识；NONE 表示按 kws_config.h 默认（KWS_ACTIVE_ENGINE）
     */
    static esp_err_t init(KwsEngineId engine = KwsEngineId::NONE);

    /** 设置主唤醒词（匹配忽略空格与标点，可随时改） */
    static esp_err_t setWakeWord(const char *word);

    /** 追加唤醒词别名（槽位满返回 ESP_ERR_NO_MEM） */
    static esp_err_t addWakeAlias(const char *word);

    /** 当前主唤醒词（归一化后） */
    static const char *wakeWord();

    /** 注册事件回调 */
    static esp_err_t setCallback(KwsEventCb cb, void *ctx);

    /** 开始常驻监听（只置标志立即返回，监督任务异步建会话） */
    static esp_err_t start();

    /** 停止监听（只置标志立即返回，可从事件回调安全调用） */
    static esp_err_t stop();

    /**
     * @brief 暂停监听
     *
     * TTS 播报期间应暂停，否则麦克风会录到喇叭声音造成自唤醒，
     * 同时也能释放会话内存（唤醒引擎与 TTS 各占一份 TLS 会话）。
     */
    static esp_err_t pause();

    /** 恢复监听（与 pause 配对） */
    static esp_err_t resume();

    /** 是否正在监听 */
    static bool isListening();

    /** 当前引擎名（诊断用，如 "xfyun-iat" / "esp-sr"） */
    static const char *engineName();

    /* ---- 引擎上报入口（各引擎实现内部调用） ---- */

    /** 引擎产出一段识别结果 */
    static void emitText(const char *text, bool isFinal);

    /** 引擎报错 */
    static void emitError(const char *msg);

private:
    static void normalize(const char *in, char *out, size_t outN);
    static const char *matchWake(const char *normText);
    static void supervisorTask(void *arg);

    static const KwsEngine *s_engine;
    static KwsEventCb s_cb;
    static void *s_cbCtx;

    static char s_wake[TEXT_MAX];
    static char s_alias[MAX_ALIAS][TEXT_MAX];
    static int s_aliasNum;

    static volatile bool s_listening;
    static volatile bool s_paused;  /*!< TTS 播报期间暂停，避免录到喇叭声音 */
    static bool s_supCreated;       /*!< 监督任务是否已创建（init 可重复调用） */
    static bool s_wakeFired;        /*!< 已唤醒，等待后续指令文本 */
    static volatile int64_t s_wakeUs; /*!< 最近一次唤醒时刻（等待提问超时用） */
};

#endif /* KWS_HUB_H */
