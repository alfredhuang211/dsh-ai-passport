// main/app_alert.h —— 提示音：待审批 / 已完成
//
// 设计取舍：**用音调而不是语音**。
//
// 曾经考虑过内置合成语音（"任务待审批"），但：
//   · 需要把语音转成 PCM 存进固件，40 字的中文语音即使压缩也要几百 KB；
//   · 语音只在听清时有用，而设备常在嘈杂环境或隔着房间；
//   · 两种状态只要**可区分**就够了 —— 急促双响 vs 舒缓单响，一耳朵就能分辨。
// 音调方案只有几千字节，且在任何环境下都能被注意到。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ALERT_NONE = 0,
    // 待审批：需要立刻处理 → QQ 双咳采样（缺失时合成兜底）
    ALERT_APPROVAL,
    // 追问/计划评审：单咳（与审批的双咳区分）
    ALERT_QUESTION,
    // 会话完成：BP 机四连哔（3200Hz，140ms 响 / 110ms 停）
    ALERT_COMPLETED,
    // 会话出错：四连哔 + 低尾音（一耳听出"出事了"）
    ALERT_ERROR,
    // 会话中断：三连哔（比完成短一截）
    ALERT_ABORTED,
    // 审批/计划已选择：上行双音（确认类反馈）
    ALERT_DECIDED,
    // 配对成功 / 失败
    ALERT_PAIR_OK,
    ALERT_PAIR_FAIL,
    // 低电量提醒
    ALERT_LOW_BATTERY,
} alert_kind_t;

/** 初始化提示音（准备 I2S 输出）。可在任意时刻调用，重复调用安全。 */
esp_err_t alert_init(void);

/**
 * 播放提示音。**非阻塞**：把播放交给独立的播放任务。
 *
 * 为什么必须非阻塞：调用方常常是 LVGL 定时器（渲染线程），
 * 在那里等几十毫秒的音频会直接拖慢界面刷新，甚至触发看门狗。
 * 播放任务串行执行，后来的请求若与正在播放的相同则跳过，避免连成一串噪音。
 */
void alert_play(alert_kind_t kind, bool enabled);

/**
 * 提示音量（百分比，**0 = 静音**）。
 *
 * ★ 用户要求：提示音不只是开关，要能调音量。档位与设置页一致：
 *   静音 0 / 低 40 / 中 70（默认）/ 高 100（见 app_settings_logic.c 的档位表）。
 *   0 等价于旧的"关闭提示音"，因此 `alert_get_setting()` 仍然可用。
 */
uint8_t alert_get_volume(void);
void alert_set_volume(uint8_t percent);

/**
 * 协议 sound.test 的名称 → 音型（"approve"/"question"/"completed"/"error"/
 * "aborted"/"decided"/"pair_ok"/"pair_fail"/"low_battery"）。
 * 未知名称返回 false，由调用方决定是否提示。
 */
bool alert_kind_from_name(const char *name, alert_kind_t *out);

/** 读取"提示音开关"的设置（= 音量是否为 0）。 */
bool alert_get_setting(void);

/** 设置提示音开关（开 = 中档，关 = 静音）。 */
void alert_set_setting(bool enabled);

#ifdef __cplusplus
}
#endif
