// main/app_voice.h —— 设备侧录音上行（按住说话 → ADPCM 流式发送）。
//
// 设计见 docs/05-语音发送-设计.md。要点：
//   · PTT：按住确定开始录音，松开结束（BSP 提供 PRESS/RELEASE）。
//   · 流式：边录边发，静态环形缓冲 + 源端丢帧，**绝不缓冲整段录音**（400KB RAM 装不下）。
//   · 编码：IMA/DVI ADPCM 4:1，块格式与主机 voice.js 的 decodeImaAdpcm 逐字节一致。
//   · 上限 30s；不做静音自动结束，录音只在松手或触达上限时结束。
//
// 线程模型（照抄 app_alert）：
//   · 采集在独立任务 app_voice_task（bsp_audio_read 阻塞，不能放 LVGL/按键任务）。
//   · app_voice_start/stop 由按键上下文调用，只发信号给任务。
//   · 结果反馈走 app_ui_post_toast（线程安全投递）。
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 初始化（建任务/队列）。失败不致命（返回错误时提示音/界面仍可用，只是没语音）。
esp_err_t app_voice_init(void);

// PTT 开始录音（按键按下时调）。已在录音中则忽略。返回是否真正启动。
bool app_voice_start(void);

// PTT 结束录音（按键松开时调）。IDLE 时无副作用。
void app_voice_stop(void);

// 是否正在录音（按键/熄屏/UI 判断用）。
bool app_voice_recording(void);

// 协议 v2：录音的目标会话 handle（进入会话页时设置，离开时清空）。
// 空串 = 未指定，主机按"最近运行的会话"兜底（兼容旧主机）。
void app_voice_set_target(const char *handle);
const char *app_voice_target(void);

// 主机回的识别结果/错误 → toast 反馈（由 app.c 收到消息时调用）。
void app_voice_on_result(const char *text);
void app_voice_on_error(const char *err);

#ifdef __cplusplus
}
#endif
