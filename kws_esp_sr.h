/**
 * @file     kws_esp_sr.h
 * @brief    唤醒引擎：ESP-SR WakeNet（离线）— 接口预留 — C++ class
 *
 * 启用前需要三步：
 *   1. 使能 PSRAM（模型必须放 PSRAM）
 *   2. 添加 espressif/esp-sr 组件 + 唤醒词模型
 *   3. 经 esp_afe + esp_gnn_wakeup 实现下面三个函数
 *
 * 自定义唤醒词需云端训练（ESP-SR Model Maker）；
 * 唤醒词可能变化时请继续使用在线引擎。
 *
 * 修改：2026-11-21 由 C 引擎实例（kws_engine_esp_sr）重构为 KwsEspSr 类
 */
#ifndef KWS_ESP_SR_H
#define KWS_ESP_SR_H

#include "kws_hub.h"

/**
 * ESP-SR 离线唤醒引擎（未实现，接口预留）
 */
class KwsEspSr {
public:
    /** 获取引擎实例（当前恒返回 ESP_ERR_NOT_SUPPORTED） */
    static const KwsEngine &engine();

private:
    static esp_err_t init();
    static esp_err_t start();
    static esp_err_t stop();
    static bool busy();
};

#endif /* KWS_ESP_SR_H */
