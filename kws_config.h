/**
 * @file     kws_config.h
 * @brief    唤醒词组件配置（PlatformIO 版：经 build_flags 覆盖，无 Kconfig）
 *
 * 修改：2026-11-21 适配 C++ class 重构：引擎枚举改用 KwsEngineId；
 *       原 Kconfig.projbuild 移除，默认值在此定义，可在
 *       platformio.ini 的 build_flags 覆盖，例如：
 *         -DCONFIG_KWS_WAKE_WORD=\"小智同学\"
 *         -DCONFIG_KWS_ENGINE_ESP_SR=1
 *       IAT 凭据未配置时自动回退到 TTS 同一应用的密钥（tts_config.h），
 *       也可运行时经 KwsXfyun::setCredentials() 注入（优先）
 */
#ifndef KWS_CONFIG_H
#define KWS_CONFIG_H

#include "kws_hub.h"
#include "tts_config.h"   /* 复用 TTS 侧已配置的讯飞密钥 */

/* 引擎选择：定义 CONFIG_KWS_ENGINE_ESP_SR 切换到 ESP-SR（未实现） */
#if defined(CONFIG_KWS_ENGINE_ESP_SR)
#define KWS_ACTIVE_ENGINE KwsEngineId::ESP_SR
#else
#define KWS_ACTIVE_ENGINE KwsEngineId::XFYUN_IAT
#endif

/* 默认唤醒词（运行时可用 KwsHub::setWakeWord 改） */
#ifndef CONFIG_KWS_WAKE_WORD
#define CONFIG_KWS_WAKE_WORD "小智小智"
#endif

/* IAT 采样率（音频格式固定 16k，此项仅记录） */
#ifndef CONFIG_KWS_IAT_SAMPLE_RATE
#define CONFIG_KWS_IAT_SAMPLE_RATE 16000
#endif

/* 静音多久判定一句话结束（ms）。值越小唤醒越快，但容易把说话中间的停顿当成结束 */
#ifndef CONFIG_KWS_IAT_EOS_MS
#define CONFIG_KWS_IAT_EOS_MS 2000
#endif

/* IAT domain：必须为 iat（v2 协议；lm 会返回 10404 no category route found） */
#ifndef CONFIG_KWS_IAT_DOMAIN
#define CONFIG_KWS_IAT_DOMAIN "iat"
#endif

/* IAT 专用凭据：未配置（空串）时回退到 TTS 同一应用的密钥 */
#ifndef CONFIG_KWS_ASR_XFYUN_APPID
#define CONFIG_KWS_ASR_XFYUN_APPID ""
#endif
#ifndef CONFIG_KWS_ASR_XFYUN_API_KEY
#define CONFIG_KWS_ASR_XFYUN_API_KEY ""
#endif
#ifndef CONFIG_KWS_ASR_XFYUN_API_SECRET
#define CONFIG_KWS_ASR_XFYUN_API_SECRET ""
#endif

#define ASR_XFYUN_APPID      CONFIG_KWS_ASR_XFYUN_APPID
#define ASR_XFYUN_API_KEY    CONFIG_KWS_ASR_XFYUN_API_KEY
#define ASR_XFYUN_API_SECRET CONFIG_KWS_ASR_XFYUN_API_SECRET

#endif /* KWS_CONFIG_H */
