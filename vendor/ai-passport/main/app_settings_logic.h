// main/app_settings_logic.h —— 系统设置的**纯逻辑**（无 NVS / 无 LVGL，可在 host 上单测）。
//
// 为什么单独拆一层：菜单模型、取值映射（"值 ↔ 档位下标"）与"是否该自动关机"的判定
// 都是纯函数，能在宿主机毫秒级验证；把它们塞进 LVGL 回调和 NVS 读写里就只能靠真机试。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define AP_SETTINGS_MAX_OPTIONS 6
#define AP_SETTINGS_ITEM_COUNT 4

typedef enum {
    AP_SETTINGS_SCREEN_OFF = 0,   // 熄屏：0 = 常亮
    AP_SETTINGS_BRIGHTNESS = 1,   // 屏幕亮度（%）：10 / 50 / 75 / 100
    AP_SETTINGS_AUTO_OFF = 2,     // 无任务自动休眠：0 = 不休眠（落地为深睡，见 main/app.c）
    AP_SETTINGS_ALERT_VOLUME = 3, // 提示音量：0 = 静音
} ap_settings_item_t;

/** 一个设置项的定义：显示名 + 各档文案 + 各档对应的值（分钟 / 音量百分比）。 */
typedef struct {
    const char *name;
    const char *labels[AP_SETTINGS_MAX_OPTIONS];
    int values[AP_SETTINGS_MAX_OPTIONS];
    int count;
} ap_settings_item_def_t;

/** 设置项定义表（只读）。 */
const ap_settings_item_def_t *ap_settings_item(ap_settings_item_t item);

/** 取值 → 档位下标；值不在表里时取**最接近**的档位（越界也安全）。 */
int ap_settings_index_of(const ap_settings_item_def_t *item, int value);

/** 环状移动档位下标（dir 为正向后、为负向前）。 */
int ap_settings_cycle(int index, int count, int dir);

/**
 * 空闲是否该自动关机。
 *
 * 任一"正在忙"的状态（任务运行 / 覆盖页打开 / 正在录音）都不关机：
 * 审批等待中关机、或用户说话说到一半关机，都是不可接受的。
 */
bool ap_settings_should_power_off(uint32_t idle_ms, int auto_off_min, bool task_running,
                                  bool overlay_open, bool recording);

/** 默认值（与 app_settings.c 的 NVS 默认一致，供测试与回落共用）。 */
#define AP_SETTINGS_DEFAULT_SCREEN_OFF_MIN 0     // 常亮
#define AP_SETTINGS_DEFAULT_AUTO_OFF_MIN 0       // 不关机
#define AP_SETTINGS_DEFAULT_BRIGHTNESS 75        // 与原先写死的背光 80% 观感接近
#define AP_SETTINGS_DEFAULT_ALERT_VOLUME 70      // 中
