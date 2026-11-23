# KWS Hub 组件 v1.0（C++ class 版）

## 一、组件概述

KWS Hub 是唤醒词检测中枢：常驻监听麦克风，把音频喂给可插拔的「唤醒引擎」；引擎产出识别文字后与用户自定义唤醒词匹配，命中即回调。唤醒后引擎继续识别，后续文字作为指令上报。

### 特性

- **引擎可插拔**：`KwsEngine` 统一形态，新增引擎只需实现接口 + 登记一行
- **讯飞在线听写引擎**：v2 IAT 流式识别（40ms/1280 字节），唤醒词完全自定义、零模型零训练，延迟约 1~2 秒
- **ESP-SR 离线引擎**：接口预留（需 PSRAM + esp-sr + 唤醒模型）
- **唤醒词归一化匹配**：忽略空格与标点，「小智， 小智」也能命中「小智小智」；支持主词 + 最多 4 个别名
- **监督任务**：常驻任务只翻标志位，start/stop 不阻塞（可从 WiFi 事件回调安全调用）；会话收尾异步完成
- **TTS 互斥**：播报期间 `pause()`，避免录到喇叭声音自唤醒，同时释放 TLS 会话内存

---

## 二、目录结构

```
kws_hub/
├── kws_hub.h / kws_hub.cpp          # KwsHub 中枢（引擎调度 + 唤醒词匹配 + 监督任务）
├── kws_xfyun.h / kws_xfyun.cpp      # KwsXfyun 讯飞在线听写引擎
├── kws_esp_sr.h / kws_esp_sr.cpp    # KwsEspSr ESP-SR 离线引擎（占位）
├── kws_config.h                     # 配置（build_flags 覆盖，无 Kconfig）
├── library.json                     # PlatformIO 库描述
└── README.md                        # 开发文档（本文件）
```

---

## 三、快速上手

```cpp
#include "kws_hub.h"
#include "kws_xfyun.h"

static void onKws(KwsEvent ev, const char *text, void *ctx) {
    switch (ev) {
    case KwsEvent::WAKE:  /* 命中唤醒词，text 为识别原文 */ break;
    case KwsEvent::TEXT:  /* 唤醒后的指令文本 */            break;
    case KwsEvent::ERROR: /* 引擎错误 */                    break;
    }
}

void app_main() {
    /* 可选：运行时注入 IAT 凭据（默认回退到 TTS 同一应用密钥） */
    // KwsXfyun::setCredentials("appid", "apikey", "apisecret");

    KwsHub::setCallback(onKws, nullptr);
    KwsHub::init();                       // 引擎按 kws_config.h 默认（讯飞 IAT）
    KwsHub::setWakeWord("小智小智");
    KwsHub::addWakeAlias("小智同学");     // 可选别名
    KwsHub::start();

    /* TTS 播报期间暂停，避免自唤醒 */
    // KwsHub::pause(); ... KwsHub::resume();
}
```

---

## 四、API 概览

### 4.1 KwsHub（静态类）

| 方法 | 说明 |
|------|------|
| `init(engine = NONE)` | 初始化并选定引擎（NONE = 按配置默认） |
| `setWakeWord(word)` | 设置主唤醒词（归一化后存储） |
| `addWakeAlias(word)` | 追加别名（最多 4 个） |
| `wakeWord()` | 当前主唤醒词 |
| `setCallback(cb, ctx)` | 注册事件回调 |
| `start() / stop()` | 开始/停止监听（只置标志，立即返回） |
| `pause() / resume()` | TTS 播报期间暂停/恢复 |
| `isListening()` | 是否正在监听 |
| `engineName()` | 当前引擎名（诊断） |
| `emitText / emitError` | 引擎上报入口（引擎实现内部调用） |

### 4.2 事件 `KwsEvent`

| 事件 | 说明 |
|------|------|
| `WAKE` | 命中唤醒词，text 为识别原文 |
| `TEXT` | 唤醒后的指令文本（一次唤醒只取第一句） |
| `ERROR` | 引擎报错，text 为错误描述 |

### 4.3 KwsXfyun（讯飞引擎）

| 方法 | 说明 |
|------|------|
| `engine()` | 引擎实例（KwsHub 内部引用） |
| `setCredentials(appId, apiKey, apiSecret)` | 运行时凭据注入（新功能） |

### 4.4 配置（kws_config.h，build_flags 覆盖）

| 宏 | 默认 | 说明 |
|----|------|------|
| `KWS_ACTIVE_ENGINE` | `KwsEngineId::XFYUN_IAT` | 定义 `CONFIG_KWS_ENGINE_ESP_SR` 切换 |
| `CONFIG_KWS_WAKE_WORD` | 小智小智 | 默认唤醒词 |
| `CONFIG_KWS_IAT_EOS_MS` | 2000 | 静音判句结束（ms） |
| `CONFIG_KWS_IAT_DOMAIN` | iat | 必须为 iat（lm 会 10404） |
| `CONFIG_KWS_ASR_XFYUN_*` | 空 | IAT 专用凭据，空则回退 TTS 密钥 |

---

## 五、技术说明（本次重构要点）

### BUG 修复

| 问题 | 修复 |
|------|------|
| 引擎初始化失败置空引擎指针，而监督任务可能已在跑 → 空指针崩溃 | 失败不置空已有引擎；监督任务加空指针防御 |

### 优化

- 会话任务 2.6KB 鉴权 URL 缓冲挪静态区，降低 8KB 任务栈压力
- IAT domain 由硬编码改为 `CONFIG_KWS_IAT_DOMAIN` 宏（默认 iat，可配）

### 新增功能

- `KwsXfyun::setCredentials()` 运行时凭据注入（优先于宏，回退链：运行时 → IAT 宏 → TTS 宏）

### 设计要点（沿用原实现的关键决策）

- **以「会话任务存活」判断重连**：用 `s_running` 会在会话正常结束后假死，后续唤醒词完全不被听
- **cJSON 探测树必须立刻释放**：否则每条返回泄漏一棵树，内部 RAM 持续下降终致听写断连
- **监督任务异步收尾**：stop() 可能从 WiFi 事件任务调用，绝不能阻塞事件循环
- **唤醒后 20 秒无提问自动回到监听**：避免卡在"等你问"导致后续喊唤醒词无反应

### 其他变更

- C API（`kws_hub_*` / `kws_engine_*`）重构为 C++ class，不保留兼容层
- 麦克风读取改经 Inmp441Mic 组件的 C 桥接（`inmp441_bridge.h`）
- 鉴权改经 `XfyunAuth` class（xfyun_auth 组件）
- 移除 Kconfig.projbuild（PlatformIO 用 build_flags）

---

## 六、版本历史

| 版本 | 日期 | 说明 |
|------|------|------|
| v1.0.0 | 2026-11-21 | 重构为 C++ class（KwsHub/KwsXfyun/KwsEspSr）；修复引擎初始化失败空指针崩溃；对接 XfyunAuth 与 Inmp441Mic 桥接；新增运行时凭据注入；Kconfig 改 build_flags |
| - | - | ESP-IDF C 版本（kws_hub_* 函数 API） |

## 许可证

MIT License
