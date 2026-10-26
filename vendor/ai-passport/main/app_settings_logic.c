// main/app_settings_logic.c —— 见头文件说明。
#include "app_settings_logic.h"

static const ap_settings_item_def_t ITEMS[AP_SETTINGS_ITEM_COUNT] = {
    [AP_SETTINGS_SCREEN_OFF] = {
        .name = "熄屏",
        .labels = { "常亮", "1 分钟", "2 分钟", "5 分钟", "10 分钟" },
        .values = { 0, 1, 2, 5, 10 },
        .count = 5,
    },
    [AP_SETTINGS_BRIGHTNESS] = {
        .name = "亮度",
        // 用户要求补一档 25%
        .labels = { "10%", "25%", "50%", "75%", "100%" },
        .values = { 10, 25, 50, 75, 100 },
        .count = 5,
    },
    [AP_SETTINGS_AUTO_OFF] = {
        // 用户要求叫「自动休眠」而不是"自动关机"：本板没有软件可控的电源锁存，
        // 实际落地是深睡（功耗最低、按任意键唤醒），叫关机容易让人以为真断了电。
        .name = "自动休眠",
        .labels = { "不休眠", "30 分钟", "1 小时", "2 小时" },
        .values = { 0, 30, 60, 120 },
        .count = 4,
    },
    [AP_SETTINGS_ALERT_VOLUME] = {
        .name = "提示音量",
        // 用户要求加一档"中高"（85%）：静音 / 低 / 中 / 中高 / 高
        .labels = { "静音", "低", "中", "中高", "高" },
        .values = { 0, 40, 70, 85, 100 },
        .count = 5,
    },
};

const ap_settings_item_def_t *ap_settings_item(ap_settings_item_t item)
{
    if ((int)item < 0 || (int)item >= AP_SETTINGS_ITEM_COUNT) return &ITEMS[0];
    return &ITEMS[item];
}

int ap_settings_index_of(const ap_settings_item_def_t *item, int value)
{
    if (!item || item->count <= 0) return 0;
    int best = 0;
    int best_diff = -1;
    for (int i = 0; i < item->count; i++) {
        const int diff = item->values[i] - value;
        const int abs_diff = diff < 0 ? -diff : diff;
        if (best_diff < 0 || abs_diff < best_diff) {
            best = i;
            best_diff = abs_diff;
        }
    }
    return best;
}

int ap_settings_cycle(int index, int count, int dir)
{
    if (count <= 0) return 0;
    int next = index + (dir >= 0 ? 1 : -1);
    if (next < 0) next = count - 1;
    if (next >= count) next = 0;
    return next;
}

bool ap_settings_should_power_off(uint32_t idle_ms, int auto_off_min, bool task_running,
                                  bool overlay_open, bool recording)
{
    if (auto_off_min <= 0) return false;      // 不关机
    if (task_running) return false;           // 有任务在跑
    if (overlay_open) return false;           // 审批/追问等覆盖页开着 —— 绝不关机
    if (recording) return false;              // 正在录音
    const uint32_t need_ms = (uint32_t)auto_off_min * 60u * 1000u;
    return idle_ms >= need_ms;
}
