// 系统设置纯逻辑的 host 测试（接进 validate.sh --static）。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "app_settings_logic.h"

static void test_items(void)
{
    assert(AP_SETTINGS_ITEM_COUNT == 4);
    const ap_settings_item_def_t *screen = ap_settings_item(AP_SETTINGS_SCREEN_OFF);
    assert(strcmp(screen->name, "熄屏") == 0);
    assert(screen->count == 5);
    assert(screen->values[0] == 0);          // 默认常亮
    assert(strcmp(screen->labels[0], "常亮") == 0);

    const ap_settings_item_def_t *auto_off = ap_settings_item(AP_SETTINGS_AUTO_OFF);
    assert(auto_off->count == 4);   // 正式档位：不休眠 / 30 分钟 / 1 小时 / 2 小时
    assert(auto_off->values[0] == 0);        // 默认不关机
    assert(auto_off->values[auto_off->count - 1] == 120);   // 最后一档 = 2 小时

    const ap_settings_item_def_t *bright = ap_settings_item(AP_SETTINGS_BRIGHTNESS);
    assert(strcmp(bright->name, "亮度") == 0);
    assert(bright->count == 5);
    assert(bright->values[0] == 10);
    assert(bright->values[1] == 25);          // 用户要求补的档
    assert(bright->values[4] == 100);
    assert(ap_settings_index_of(bright, AP_SETTINGS_DEFAULT_BRIGHTNESS) == 3);   // 75%

    const ap_settings_item_def_t *vol = ap_settings_item(AP_SETTINGS_ALERT_VOLUME);
    assert(vol->count == 5);   // 静音 / 低 / 中 / 中高 / 高
    assert(vol->values[0] == 0);             // 静音
    assert(strcmp(vol->labels[2], "中") == 0);
    assert(strcmp(vol->labels[3], "中高") == 0);
    assert(vol->values[3] == 85);
    assert(vol->values[2] == AP_SETTINGS_DEFAULT_ALERT_VOLUME);
    printf("== 设置项表 ==  通过\n");
}

static void test_index_of(void)
{
    const ap_settings_item_def_t *vol = ap_settings_item(AP_SETTINGS_ALERT_VOLUME);
    assert(ap_settings_index_of(vol, 0) == 0);
    assert(ap_settings_index_of(vol, 70) == 2);
    assert(ap_settings_index_of(vol, 100) == 4);
    assert(ap_settings_index_of(vol, 85) == 3);    // 中高
    assert(ap_settings_index_of(vol, 65) == 2);   // 最接近"中"
    assert(ap_settings_index_of(vol, 30) == 1);   // 最接近"低"
    assert(ap_settings_index_of(vol, 999) == 4);  // 越界 → 最高档
    assert(ap_settings_index_of(vol, -5) == 0);   // 负值 → 最低档

    const ap_settings_item_def_t *sc = ap_settings_item(AP_SETTINGS_SCREEN_OFF);
    assert(ap_settings_index_of(sc, 3) == 2);     // 2 分钟
    printf("== 取值映射 ==  通过\n");
}

static void test_cycle(void)
{
    assert(ap_settings_cycle(0, 4, 1) == 1);
    assert(ap_settings_cycle(3, 4, 1) == 0);      // 环状
    assert(ap_settings_cycle(0, 4, -1) == 3);
    assert(ap_settings_cycle(2, 4, -1) == 1);
    assert(ap_settings_cycle(0, 0, 1) == 0);      // 空表安全
    printf("== 档位循环 ==  通过\n");
}

static void test_power_off(void)
{
    const uint32_t min = 60u * 1000u;
    assert(!ap_settings_should_power_off(999u * min, 0, false, false, false));       // 不关机
    assert(!ap_settings_should_power_off(29u * min, 30, false, false, false));       // 未到点
    assert(ap_settings_should_power_off(30u * min, 30, false, false, false));        // 到点
    assert(!ap_settings_should_power_off(99u * min, 30, true, false, false));        // 有任务
    assert(!ap_settings_should_power_off(99u * min, 30, false, true, false));        // 覆盖页开着
    assert(!ap_settings_should_power_off(99u * min, 30, false, false, true));        // 正在录音
    assert(ap_settings_should_power_off(120u * min, 120, false, false, false));      // 2 小时档
    printf("== 空闲关机判定 ==  通过\n");
}

int main(void)
{
    test_items();
    test_index_of();
    test_cycle();
    test_power_off();
    printf("== 系统设置逻辑测试全部通过 ==\n");
    return 0;
}
