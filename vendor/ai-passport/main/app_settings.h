// main/app_settings.h —— 设备端系统设置的持久化（NVS）。
//
// 与 app_settings_logic 的分工：逻辑层定义"有哪些项、档位怎么走、何时该休眠"（纯函数、可单测）；
// 本层只负责"把选定的值存住、下次开机读回来"。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_settings_logic.h"
#include "esp_err.h"

typedef struct {
    int screen_off_min;   // 熄屏分钟数；0 = 常亮
    int brightness;       // 屏幕亮度百分比（10/50/75/100）
    int auto_off_min;     // 无任务自动休眠分钟数；0 = 不休眠
    int alert_volume;     // 提示音量百分比；0 = 静音
} ap_settings_t;

/**
 * 从 NVS 读设置；键缺失或读取失败时保留默认值（不阻塞开机）。
 *
 * 要求 `nvs_flash_init()` 已经调用过（app.c 在启动早期就做了）。
 */
esp_err_t ap_settings_init(void);

/**
 * "已休眠"标记：上一次是**自动休眠**进的深睡。
 *
 * 用途：开机早期判定"这次启动是不是用户按键唤醒的" —— 不是就重新睡回去，
 * 否则用户会看到"设了自动休眠，到点又自己重启了"（真机反馈）。
 */
bool ap_settings_is_powered_off(void);
esp_err_t ap_settings_set_powered_off(bool powered_off);

/** 当前设置（只读）。未初始化时返回默认值。 */
const ap_settings_t *ap_settings_get(void);

/**
 * 把某个设置项设成"值"（分钟 / 音量百分比），并写入 NVS。
 *
 * 值会先按档位表归一（`ap_settings_index_of` → 档位值），避免存进表外的野值。
 * NVS 写失败只记日志：内存里的值仍然生效，本次开机可用。
 */
esp_err_t ap_settings_set_value(ap_settings_item_t item, int value);
