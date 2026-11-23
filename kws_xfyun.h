/**
 * @file     kws_xfyun.h
 * @brief    唤醒引擎：讯飞在线语音听写（IAT）+ 唤醒词文本匹配 — C++ class
 *
 * 协议采用最通用、每个讯飞应用默认开通的「语音听写（流式版）」v2：
 *   wss://iat-api.xfyun.cn/v2/iat
 *   首帧带 common/business/data，之后按 40ms/1280 字节流式发音频，末帧 status=2。
 *   返回 data.result.ws[].cw[].w 为识别文本，data.result.ls=true 为整句结束。
 *
 * 说明：早期误用的 iat.xf-yun.com/v1 端点在新版按 domain 路由，domain=lm
 * 会返回 10404 "no category route found"，故改用 v2 + domain=iat。
 *
 * 修改：2026-11-21 由 C 引擎实例（kws_engine_xfyun）重构为 KwsXfyun 类；
 *       鉴权改经 XfyunAuth class；新增运行时凭据注入 setCredentials
 */
#ifndef KWS_XFYUN_H
#define KWS_XFYUN_H

#include "kws_hub.h"

/**
 * 讯飞在线听写唤醒引擎
 */
class KwsXfyun {
public:
    /** 获取引擎实例（填进 KwsHub 引擎表） */
    static const KwsEngine &engine();

    /**
     * @brief 运行时注入 IAT 凭据（新功能）
     *
     * 优先于编译期宏；未设置或为空时按原回退链：
     * CONFIG_KWS_ASR_XFYUN_* → TTS 同一应用密钥。
     */
    static void setCredentials(const char *appId, const char *apiKey, const char *apiSecret);

    /** 生效凭据（运行时注入 → IAT 宏 → TTS 宏） */
    static const char *effAppId();
    static const char *effApiKey();
    static const char *effApiSecret();

private:
    static esp_err_t init();
    static esp_err_t start();
    static esp_err_t stop();
    static bool busy();

    static char s_appId[32];
    static char s_apiKey[64];
    static char s_apiSecret[64];
    static bool s_credsSet;
};

#endif /* KWS_XFYUN_H */
