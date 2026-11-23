/**
 * @file     kws_esp_sr.cpp
 * @brief    ESP-SR WakeNet 离线唤醒引擎（占位实现）
 *
 * 修改：2026-11-21 重构为 KwsEspSr 类
 */
#include "esp_log.h"

#include "kws_esp_sr.h"

static const char *TAG = "KWS_ESP_SR";

esp_err_t KwsEspSr::init()
{
    ESP_LOGE(TAG, "ESP-SR engine not enabled yet: need PSRAM + esp-sr + wake model");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t KwsEspSr::start() { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t KwsEspSr::stop()  { return ESP_ERR_NOT_SUPPORTED; }
bool KwsEspSr::busy()       { return false; }

const KwsEngine &KwsEspSr::engine()
{
    static const KwsEngine eng = {
        .name  = "esp-sr",
        .init  = KwsEspSr::init,
        .start = KwsEspSr::start,
        .stop  = KwsEspSr::stop,
        .busy  = KwsEspSr::busy,
    };
    return eng;
}
