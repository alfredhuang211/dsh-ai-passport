// main/app_ui.h —— 联机状态界面。
//
// 上游规范（AGENTS.md）明确要求：二次开发**必须重新设计并实现独立 UI**，
// 禁止沿用 demo 测试菜单；仅改名换色不算重新设计。因此这里的界面是全新的：
// 顶部状态条 + 中部任务台 + 底部键位提示，与 demo 的九宫格卡片菜单完全不同。
//
// 线程约束：所有函数都必须在 LVGL 任务里或持有 bsp_lvgl_lock() 时调用。
// 唯一例外是 app_ui_post_toast()，它只投递数据，由 LVGL 定时器消费。
#pragma once

#include "app_link.h"
#include "bsp_button.h"

#include "lvgl.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 界面统一使用**自己生成的**字体子集（main/fonts/app_ui_font_16.c）。
//
// 为什么不用 LVGL 内置的 lv_font_source_han_sans_sc_16_cjk：
// 它的字符集是 LVGL 示例里随手挑的一批字，**不是**按应用文案选的。
// 实测本界面渲染的 48 个中文字里，它缺 23 个（"剩务发后对开态择按无显暂确秒脑话详这连选里长闲"），
// 那些位置直接显示成方框。
//
// 本字体由 tools/make-font-subset.py 从思源黑体（SIL OFL）生成，
// 字形集从源码里的字符串字面量**自动提取**，因此文案改了只要重跑脚本即可，
// 不会因为手工维护列表而漂移（漂移的表现就是"又出现方框"，且要到真机才发现）。
#define APP_UI_FONT (&app_ui_font_16)

// 由 lv_font_conv 生成（见 main/fonts/app_ui_font_16.c）
extern const lv_font_t app_ui_font_16;

#ifdef __cplusplus
extern "C" {
#endif

// 应用入口。由上游 main.c 的 app_main() 调用（本应用对上游的唯一侵入点）。
void passport_app_main(void);

// 界面一次能显示的任务条数。屏幕只有 240×320，超过这个数就得滚动，
// 而滚动在只有三个按键的设备上体验很差 —— 改为限流，更多任务由主机侧翻页。
// 导出这个常量，避免 app.c 与 app_ui.c 各写一个数字后对不上。

// 建立一个任务条目，供列表显示。
// ── 按键标识 ──────────────────────────────────────────────────────────────
//
// ★ 界面层必须用这些名字，不能写魔法数字。
//   之前 app_ui.c 里写的是 `event == 1` / `event == 3` / `btn == 2`，
//   虽然与 BSP 的枚举值**恰好**一致，但那只是巧合 —— 一旦 BSP 调整枚举顺序，
//   界面会静默错位，而且现象（"按键没反应"）与真正的驱动故障无法区分。
//   这里显式映射到 BSP 的枚举，让编译期就能发现不一致。
#define APP_UI_BTN_UP   BSP_BTN_UP
#define APP_UI_BTN_DOWN BSP_BTN_DOWN
#define APP_UI_BTN_OK   BSP_BTN_OK
#define APP_UI_EV_PRESS  BSP_BTN_PRESS
#define APP_UI_EV_CLICK  BSP_BTN_CLICK
#define APP_UI_EV_DOUBLE BSP_BTN_DOUBLE
#define APP_UI_EV_LONG   BSP_BTN_LONG
#define APP_UI_EV_RELEASE BSP_BTN_RELEASE   // PTT 按住说话的松手

typedef struct {
    char session_id[40]; // 会话 id。设备靠它向主机请求详情、上报选中项 ——
                         // 仅靠列表下标是不够的：主机侧的列表可能因新任务而重排。
    char title[64];      // 任务标题（UTF-8）
    char status[16];     // idle/running/done/error/aborted/waiting
    char tool[24];       // 当前工具名，可为空
    uint16_t step;       // 当前 step，0 表示未知
    bool unread;         // 是否有未读的完成提醒
    bool busy;           // 该任务是否正在运行（用于呼吸点）
} ap_task_view_t;

// 创建并载入主界面。必须在持有 LVGL 锁时调用。
void app_ui_create(void);

// 销毁界面（退出应用时）。
void app_ui_destroy(void);

// 链路状态变化时调用，刷新顶部状态条。线程安全（投递给 LVGL 定时器后生效）。
void app_ui_on_link_state(ap_link_state_t state);

/**
 * 设置当前任务状态（精简版的核心接口）。
 *
 * ★ 状态由**主机聚合**后下发，设备不做任何判断。理由是状态推导需要知道
 *   "有几个会话在跑""有没有审批挂起""刚才是否结束"，这些只有主机侧才知道；
 *   设备端猜出来的状态一定会与主机不一致，而一个会撒谎的状态指示器比没有更糟。
 *
 * @param state 四态之一（空闲/运行中/待审批/已完成）
 * @param title 可选的短标题（显示在状态下方），传 NULL 或空串则显示状态说明
 */
void app_ui_set_task_state(ap_task_state_t state, const char *title);

/** 立刻按当前设置应用屏幕亮度（设置页改档位时用，不用等下次亮屏）。 */
void app_ui_apply_brightness(void);

// 录音状态变化（由 app_voice.c 调用）：结束录音时应用录音期间被缓存的状态。
void app_ui_set_recording(bool recording);

// 更新设备自检信息（电量、RSSI、MTU、固件版本）。线程安全。
void app_ui_set_device_info(int battery_percent, int8_t rssi, uint16_t mtu);

// 设置任务列表（整表替换）。线程安全。

// 就地更新单个任务的状态（增量推送用）。线程安全。

// 显示一条短暂提示（完成提醒、错误提示等）。线程安全。
// urgent=true 时用更醒目的配色，并由调用方决定是否响提示音。
void app_ui_post_toast(const char *text, bool urgent);

// 粘性 toast：不自动消失，直到下一个 toast 或 app_ui_hide_toast()。
// 用于"开始说话"这类需要持续显示的状态提示（按住说话期间一直可见）。
void app_ui_post_toast_sticky(const char *text, bool urgent);

// 显式隐藏提示条（清除粘性 toast）。
void app_ui_hide_toast(void);

// 当前选中的任务下标（无任务时为 -1）。

/** 当前列表里的任务条数（诊断用）。 */

// 取出选中任务的 sessionId。写入 out（可为 NULL 查询长度）。
// 返回 false 表示没有选中项或数据不可用。

// 按键事件入口。必须在 LVGL 任务里调用（app_main 负责加锁）。
// 返回 true 表示该事件已被界面消费。
bool app_ui_handle_key(int btn, int event);

#ifdef __cplusplus
}
#endif
