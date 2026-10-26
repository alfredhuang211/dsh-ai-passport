// main/app_ui.c —— 设备主界面（精简版：只显示当前任务状态）
//
// ══════════════════════════════════════════════════════════════════════════
//  为什么只剩一屏
// ══════════════════════════════════════════════════════════════════════════
//  上一版有任务列表、详情页、余额页。实际使用后确认：**任务列表在 240×320 上
//  既看不全也读不快**，用户真正需要的是"现在要不要我管一下"。因此精简成
//  一个状态词 + 一个大色块：
//
//      空闲     灰    没有任务在跑
//      运行中   青    任务正在执行，不用管
//      待审批   琥珀  ★ 需要你按键，会响提示音
//      已完成   绿    刚跑完，会响提示音
//
//  设计的核心是**状态一眼可辨**：颜色 + 大字 + 全屏占比。
//  设备可能被放在桌角，扫一眼就知道要不要伸手。
//
// ══════════════════════════════════════════════════════════════════════════
//  交互只有两件事
// ══════════════════════════════════════════════════════════════════════════
//  · 待审批时：上/下选「运行一次 / 拒绝」，确定提交（覆盖层）
//  · 其余时候：下键长按看余额；按住确定说话（PTT，松开发送）
//
// ══════════════════════════════════════════════════════════════════════════
//  三条来自真机返工的教训（改这个文件前务必读完）
// ══════════════════════════════════════════════════════════════════════════
//  1. **含中文的文本不能用 8px 字体**。app_ui_font_8 行高只有 8，而中文字形
//     12~13px 高，LVGL 按行高裁剪，剩下的就是一个方块。中文一律 ≥16px。
//
//  2. **行位置必须由字体行高算出，不能写死像素**。曾给底栏第二行写死 y=19，
//     而中文行高 24px > 19px，两行文字直接压在一起。
//
//  3. **元素坐标要么来自 UI_* 宏，要么来自行高计算**，不要出现魔法数字。
//     版面由 tools/verify-layout.py 预先验证，不靠"改完拍照看看"迭代 ——
//     设备没有截图能力，拍照一轮要十几分钟。
//
// ══════════════════════════════════════════════════════════════════════════
//  线程模型
// ══════════════════════════════════════════════════════════════════════════
//  其他任务只写 volatile 的"待处理"结构，LVGL 定时器统一消费并更新控件。
//  **绝不跨任务直接操作 LVGL 对象**（LVGL 非线程安全）。
#include "app_ui.h"

#include "app_proto.h"
#include "app_conn_logic.h"
#include "app_link.h"
#include "app_screens.h"
#include "app_settings.h"
#include "app_ui_theme.h"
#include "app_voice.h"
#include "utf8_clamp.h"

#include <stdio.h>
#include <string.h>

#include "bsp_display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

static const char *TAG = "app_ui";

// 熄屏：无操作 5 分钟后关背光。任何状态变化都会重新亮屏 ——
// 熄屏只是省电，不该让用户错过"待审批"这种需要立刻响应的状态。
// 熄屏时长不再写死：由设置页决定（0 = 常亮），见 app_ui_tick 里的读取
// 背光百分比改由设置页决定（默认 75%，见 app_settings_logic 的档位表）

// ── 界面对象 ──────────────────────────────────────────────────────────────

static lv_obj_t *s_scr;

// 状态条（顶部细条：链路状态 + 应用名）
// 链路信号格（替代旧的"已连接/未连接"文字与色块，用户要求）
#define LINK_BARS 4
static lv_obj_t *s_link_bars[LINK_BARS];

// 电量显示（右上角，带百分比）。值由 app_ui_set_device_info 喂入（app 任务、
// 10 秒一次），LVGL 更新在 ui_tick 里做 —— 与其它跨任务投递同一套脏标志模式。
#define BATT_UNKNOWN (-2)
static lv_obj_t *s_batt_shell;
static lv_obj_t *s_batt_fill;
static lv_obj_t *s_batt_tip;
static lv_obj_t *s_batt_text;
static volatile int s_pending_battery = BATT_UNKNOWN;
static volatile bool s_batt_dirty = false;
static lv_obj_t *s_app_name;

// 状态主体（占据屏幕绝大部分）
static lv_obj_t *s_state_block;   // 大色块
static lv_obj_t *s_state_text;    // 状态大字
static lv_obj_t *s_state_hint;    // 一行补充说明（任务标题 / 提示）

static lv_obj_t *s_footer_l1;     // 底栏提示：第一行
static lv_obj_t *s_footer_l2;     // 底栏提示：第二行

static lv_obj_t *s_toast;         // 短暂通知
static lv_obj_t *s_toast_text;

static lv_timer_t *s_timer;
static lv_timer_t *s_toast_timer;

// ── 状态 ──────────────────────────────────────────────────────────────────

static ap_task_state_t s_state = AP_TASK_STATE_IDLE;
/** 主机随状态一起下发的短标题，可为空。 */
static char s_state_title[64];

static bool s_screen_on = true;
static uint32_t s_last_activity_ms = 0;

/**
 * 「待审批」状态等待审批请求的兜底超时。
 *
 * 主机先推状态、再发审批请求。正常情况下两者相隔几十毫秒，用户察觉不到。
 * 但如果**请求那一条丢了**（BLE 通知不保证送达），设备就会永远显示
 * 「等待授权请求」而没有任何可操作的东西 —— 提示是真的，但状态卡死了。
 *
 * 主机侧的审批超时（默认 30 秒）会走"按拒绝上报"并把状态收回，
 * 所以这里只需给一个略长的上限，把"主机也没能收回"这种极端情况兜住。
 */
#define APPROVAL_WAIT_TIMEOUT_MS 40000
static uint32_t s_waiting_since_ms = 0;

// 待处理更新（其他任务写，LVGL 定时器读）
static volatile bool s_state_dirty = true;
static volatile ap_task_state_t s_pending_state = AP_TASK_STATE_IDLE;
static char s_pending_title[64];
static volatile bool s_link_dirty = true;
static volatile ap_link_state_t s_pending_link = AP_LINK_OFF;
// 上一次消费到的链路状态：用于判断"是否从已连接掉到断开"（掉线就回主页）。
static ap_link_state_t s_last_link = AP_LINK_OFF;
// 录音期间被缓存的状态变化（见 app_ui_set_task_state / app_ui_set_recording）。
static ap_task_state_t s_deferred_state = AP_TASK_STATE_IDLE;
static char s_deferred_title[64];
static volatile bool s_deferred_state_pending = false;

static volatile bool s_toast_dirty = false;
// 语音任务等非 LVGL 任务请求隐藏 toast（只置标志，由 ui_tick 落地）。
static volatile bool s_toast_hide_dirty = false;
// ★ 512 字节而不是 80：识别结果是动态文本（一句话可几十个汉字），80 字节
//   （约 26 个汉字）会把长句硬截成半截，配合下面的 UTF-8 安全截断才不会出方块。
static char s_pending_toast[512];
static volatile bool s_pending_toast_urgent = false;
// ★ 粘性 toast 标志：true 时 toast_show_sticky 不自动消失（按住说话期间一直可见）。
static volatile bool s_pending_toast_sticky = false;

// ── 状态 → 视觉 ───────────────────────────────────────────────────────────

/**
 * 任务状态 → 显示文案与配色。
 *
 * 「待审批」与「已完成」用最醒目的两种色（琥珀 / 绿），因为它们是需要
 * 用户注意的状态；「运行中」用青色（说明在动，但不用管）；「空闲」用灰。
 */
static void state_style(ap_task_state_t st, const char **text, const char **hint, uint32_t *color)
{
    switch (st) {
    case AP_TASK_STATE_RUNNING:
        *text = "运行中";
        *hint = "任务正在执行";
        *color = UI_INFO;
        break;
    case AP_TASK_STATE_WAITING_APPROVAL:
        *text = "待审批";
        *hint = "按确定键处理";
        *color = UI_WARN;
        break;
    case AP_TASK_STATE_COMPLETED:
        *text = "已完成";
        *hint = "任务刚刚结束";
        *color = UI_ACCENT;
        break;
    case AP_TASK_STATE_IDLE:
    default:
        *text = "空闲";
        *hint = "没有正在执行的任务";
        *color = UI_DIM;
        break;
    }
}

/**
 * 清洗来自主机的文本：换行折成空格，丢弃其它控制字符。
 *
 * 保留中文 —— 字体子集已覆盖界面文案与常见汉字。字体没覆盖到的字**不做降级处理**：
 * 那说明字体子集没跟上文案，由 tools/make-fonts.py 的覆盖率校验在构建期拦住，
 * 而不是运行期悄悄改成 '?'（那会让"缺字"这个真问题被掩盖）。
 */
static void clean_text(const char *src, char *dst, size_t dst_size)
{
    size_t out = 0;
    if (dst_size == 0) return;
    for (const unsigned char *p = (const unsigned char *)(src ? src : ""); *p && out + 1 < dst_size; p++) {
        if (*p == '\n' || *p == '\r' || *p == '\t') {
            dst[out++] = ' ';
        } else if (*p >= 0x20) {
            dst[out++] = (char)*p;
        }
    }
    dst[out] = '\0';
}



// ── 熄屏 ──────────────────────────────────────────────────────────────────

/** 记录一次"有意义的动静"，用于推迟熄屏。 */
static void note_activity(void)
{
    s_last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

static void screen_set(bool on)
{
    if (s_screen_on == on) return;
    s_screen_on = on;
    // 只关背光、不关屏：重新点亮时无需重建界面，也没有闪烁。
    bsp_display_backlight(on ? (uint8_t)ap_settings_get()->brightness : 0);
    if (on) note_activity();
    ESP_LOGI(TAG, "屏幕%s", on ? "点亮" : "熄灭（无操作到设置时长）");
}

/** 亮屏（若已亮则只刷新活动时间）。状态变化时应调用。 */
static void screen_wake(void)
{
    note_activity();
    screen_set(true);
}

// ── 刷新 ──────────────────────────────────────────────────────────────────

static void refresh_state(void)
{
    if (!s_scr) return;

    const char *text = "空闲";
    const char *hint = "";
    uint32_t color = UI_DIM;
    state_style(s_state, &text, &hint, &color);

    lv_label_set_text(s_state_text, text);
    lv_obj_set_style_text_color(s_state_text, lv_color_hex(color), 0);
    // 大色块作为背景强调：小屏上色块的辨识度远高于文字颜色
    lv_obj_set_style_bg_color(s_state_block, lv_color_hex(color), 0);

    // 补充行：优先显示主机下发的任务标题，没有就显示状态说明
    lv_label_set_text(s_state_hint, s_state_title[0] ? s_state_title : hint);
    lv_obj_set_style_text_color(s_state_hint, lv_color_hex(UI_DIM), 0);

    // ── 底栏提示 ──────────────────────────────────────────────────────────
    //
    // ★ 这里的一条规矩：**提示只能写"按了确实会发生的事"**。
    //
    //   原先在「待审批」时写的是"确定键处理审批"，而主页的确定键**什么都不做**
    //   （审批页是收到主机请求时自动弹出的，主页按键并不参与）。
    //   一句做不到的指引比没有指引更糟 —— 用户会反复按、以为设备坏了。
    //
    //   因此按"此刻真正可用的操作"来写：
    //     · 审批挂起但审批页尚未出现 → 明确说"等待授权请求"，让用户知道该等
    //     · 其余情况 → 两行独立展示（24px 行框 + 4px 行间隙），不折行不挤压
    switch (s_state) {
    case AP_TASK_STATE_WAITING_APPROVAL:
        // 审批页会由主机请求自动弹出；在它出现之前主页没有可执行动作。
        lv_label_set_text(s_footer_l1, "等待授权请求");
        lv_obj_set_style_text_color(s_footer_l1, lv_color_hex(UI_WARN), 0);
        lv_obj_set_pos(s_footer_l1, 0, UI_CENTER_Y(UI_FOOTER_H, UI_LINE_H_CJK));
        lv_label_set_text(s_footer_l2, "");
        break;
    default:
        // 底栏两行都用 16px 中文：一行能放约 13 个字，才装得下三条手势提示
        lv_label_set_text(s_footer_l1, "长按上键设置 下键看余额");
        lv_obj_set_style_text_color(s_footer_l1, lv_color_hex(UI_DIM), 0);
        lv_obj_set_pos(s_footer_l1, 0, UI_FOOTER_LINE1_Y);
        lv_label_set_text(s_footer_l2, "按住确定说话");
        lv_obj_set_style_text_color(s_footer_l2, lv_color_hex(UI_DIM), 0);
        break;
    }
}

static void refresh_signal_bars(void);

static void refresh_link(void)
{
    if (!s_scr) return;
    refresh_signal_bars();

    // 顶栏只留应用名：连接状态改由**信号格**表达（用户要求），
    // 文字状态（已连接/未连接…）不再占位置。
    lv_label_set_text(s_app_name, "AI 通行证");

    if (s_pending_link == AP_LINK_CONNECTED || s_pending_link == AP_LINK_READY) {
        refresh_state();   // 恢复正常的任务提示
    } else {
        // 未连接在主界面上给明确提示（用户要求）。此时按住说话/看余额都无效，
        // 底栏也只写做得到的事。
        lv_label_set_text(s_state_hint, "蓝牙未连接");
        lv_obj_set_style_text_color(s_state_hint, lv_color_hex(UI_ERR), 0);
        lv_label_set_text(s_footer_l1, "等待蓝牙连接");
        lv_obj_set_style_text_color(s_footer_l1, lv_color_hex(UI_WARN), 0);
        lv_obj_set_pos(s_footer_l1, 0, UI_CENTER_Y(UI_FOOTER_H, UI_LINE_H_CJK));
        lv_label_set_text(s_footer_l2, "");
    }
}

/**
 * 信号格：4 格递增方块，格数 = RSSI 强弱（ap_rssi_level）。
 * 颜色随链路阶段变：READY 绿 / 握手中琥珀 / 未连接全部熄灭。
 * 由 ui_tick 每秒刷一次（RSSI 缓存本身 2 秒一采，见 app_link）。
 */
static void refresh_signal_bars(void)
{
    const bool linked = (s_pending_link == AP_LINK_CONNECTED || s_pending_link == AP_LINK_READY);
    const int level = linked ? ap_rssi_level(ap_link_rssi()) : 0;
    const uint32_t color = (s_pending_link == AP_LINK_READY) ? UI_ACCENT
                         : linked ? UI_WARN : UI_LINE;
    for (int i = 0; i < LINK_BARS; i++) {
        lv_obj_set_style_bg_color(s_link_bars[i], lv_color_hex((i < level) ? color : UI_LINE), 0);
    }
}

/** 电量：图标内条宽 + 百分比文字 + 颜色分档（≥30 绿 / 15-29 琥珀 / <15 红）。 */
static void refresh_battery(void)
{
    if (!s_batt_text) return;
    const int pct = s_pending_battery;
    if (pct < 0) {
        // 未知（首次采样前 / 电池芯片读失败）：显示 "--"，不显示假的 0%
        lv_label_set_text(s_batt_text, "--");
        lv_obj_set_style_text_color(s_batt_text, lv_color_hex(UI_DIM), 0);
        lv_obj_set_width(s_batt_fill, 0);
        return;
    }
    const int p = pct > 100 ? 100 : pct;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", p);
    lv_label_set_text(s_batt_text, buf);
    const uint32_t color = (p >= 30) ? UI_ACCENT : (p >= 15) ? UI_WARN : UI_ERR;
    lv_obj_set_style_text_color(s_batt_text, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(s_batt_fill, lv_color_hex(color), 0);
    lv_obj_set_width(s_batt_fill, (p * 16 + 50) / 100);   // 外壳内宽 16px
}

// ── 提示条 ────────────────────────────────────────────────────────────────

static void toast_timeout(lv_timer_t *timer)
{
    (void)timer;
    if (s_toast) lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
}

// ★ 粘性 toast：不自动消失，直到下一个 toast 或 app_ui_hide_toast() 显式隐藏。
//   用于"开始说话"这类需要持续显示的状态提示（按住说话期间一直可见）。
static void toast_show_sticky(const char *text, bool urgent)
{
    if (!s_toast) return;
    char clamped[512];
    utf8_copy_clamped(text, clamped, sizeof(clamped));
    char clean[512];
    clean_text(clamped, clean, sizeof(clean));
    lv_label_set_text(s_toast_text, clean);
    lv_obj_set_style_border_color(s_toast, lv_color_hex(urgent ? UI_ERR : UI_ACCENT), 0);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast);
    // 不启动自动隐藏定时器 —— 粘性 toast 一直显示到被显式隐藏或新 toast 替换。
    if (s_toast_timer) { lv_timer_delete(s_toast_timer); s_toast_timer = NULL; }
}

static void toast_show(const char *text, bool urgent)
{
    if (!s_toast) return;
    // ★ 先按字符边界做 UTF-8 安全截断（而非按字节硬切），再清洗控制字符。
    //   顺序不能反：clean_text 逐字节搬运，若源串末尾本来就是半个汉字，
    //   搬运后仍然是半截，LVGL 渲染时就成了方块。见 utf8_clamp.c 的说明。
    char clamped[512];
    utf8_copy_clamped(text, clamped, sizeof(clamped));
    char clean[512];
    clean_text(clamped, clean, sizeof(clean));
    lv_label_set_text(s_toast_text, clean);
    // 用左边框颜色区分紧急程度：比改背景色更醒目，也不刺眼
    lv_obj_set_style_border_color(s_toast, lv_color_hex(urgent ? UI_ERR : UI_ACCENT), 0);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast);
    if (s_toast_timer) lv_timer_delete(s_toast_timer);
    s_toast_timer = lv_timer_create(toast_timeout, urgent ? 4000 : 2500, NULL);
}

/** 显式隐藏提示条（用于清除"开始说话"这类粘性提示）。 */
void app_ui_hide_toast(void)
{
    // ★ 本函数会被**语音任务**调用（录音收尾 app_voice.c），因此绝不能在这里碰 LVGL。
    //   真机事故（2026-10-06）：这里直接 lv_obj_add_flag + lv_timer_delete，与 LVGL 任务
    //   并发访问内部链表 → 损坏 → lv_inv_area 死循环 → 整机冻死。
    //   现象："按住说话说完，一松手就死机"（收尾正好是 VAD 判定那一刻）。
    //   现在只置标志，真正的隐藏在 LVGL 任务里的 ui_tick 消费。
    s_toast_hide_dirty = true;
}

/** 真正隐藏 toast（**只在 LVGL 任务里调用**）。 */
static void toast_hide_now(void)
{
    if (s_toast) lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    if (s_toast_timer) { lv_timer_delete(s_toast_timer); s_toast_timer = NULL; }
}

// ── LVGL 定时器 ───────────────────────────────────────────────────────────

void app_screens_consume_pending(void);

static void ui_tick(lv_timer_t *timer)
{
    (void)timer;

    // 覆盖页的跨任务投递与结果卡计时都挂在这一个定时器上：它们访问同一批 LVGL 对象，
    // 本来就必须串行；多开定时器只是徒增调度开销。
    // （审批页的倒计时已按用户要求删除，这里不再需要它的 tick。）
    app_screens_consume_pending();
    app_screens_tick_voice((uint32_t)(esp_timer_get_time() / 1000));
    app_screens_tick_question((uint32_t)(esp_timer_get_time() / 1000));

    // 电量格每秒刷一次：RSSI 缓存 2 秒一采，太快刷也只是空转。
    {
        static uint32_t s_signal_next_ms = 0;
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if ((int32_t)(now - s_signal_next_ms) >= 0) {
            s_signal_next_ms = now + 1000;
            refresh_signal_bars();
        }
    }

    if (s_batt_dirty) {
        s_batt_dirty = false;
        refresh_battery();
    }

    if (s_link_dirty) {
        s_link_dirty = false;
        // 蓝牙断开 → 默认返回主页：收起余额/审批等覆盖层。
        // 链路一旦离开 CONNECTED/READY（掉到 OFF/ADVERTISING/FAILED 就是断了），
        // 覆盖层留着也没意义（审批发不出去、余额拉不到），不如回到主页。
        const ap_link_state_t prev_link = s_last_link;
        s_last_link = s_pending_link;
        const bool was_up = (prev_link == AP_LINK_CONNECTED || prev_link == AP_LINK_READY);
        const bool now_up = (s_pending_link == AP_LINK_CONNECTED || s_pending_link == AP_LINK_READY);
        if (was_up && !now_up) {
            // ★ 配对码页例外：未配对时链路本来就不会进 READY，一旦按"断开"收掉，
            //   用户就看不到码、也就没法配对（真机反馈"设备上没有确认配对码的地方"）。
            //   它只在用户主动关闭或配对成功时收起。
            if (app_screens_current() != AP_SCREEN_NONE &&
                app_screens_current() != AP_SCREEN_PAIR) {
                ESP_LOGI(TAG, "蓝牙断开，覆盖层返回主页");
                // ★ 必须 hide(当前页)：之前写的是 hide(AP_SCREEN_NONE)，
                //   那是空操作（不匹配任何覆盖层对象），断开后覆盖层一直留在屏幕上。
                app_screens_hide(app_screens_current());
                // 协议 v2：链路断开后会话内容作废，别把上次连接的数据当在线状态。
                app_screens_clear_sessions();
                app_voice_set_target("");
            }
        }
        refresh_link();
    }
    if (s_state_dirty) {
        s_state_dirty = false;
        const ap_task_state_t prev = s_state;
        s_state = s_pending_state;
        snprintf(s_state_title, sizeof(s_state_title), "%s", s_pending_title);
        // 记录进入「待审批」的时刻，用于下面的兜底超时
        if (s_state == AP_TASK_STATE_WAITING_APPROVAL && prev != AP_TASK_STATE_WAITING_APPROVAL) {
            s_waiting_since_ms = (uint32_t)(esp_timer_get_time() / 1000);
        }
        refresh_state();
        // ★ 状态变化就亮屏。熄屏是省电，不是"屏蔽通知"——
        //   待审批与已完成必须让用户看得见，否则熄屏就成了漏掉审批的原因。
        if (s_state != prev) {
            screen_wake();
            // ★ 状态变化 → 回主页（真机返工记录）：收起余额覆盖层。
            //   余额是"随时可查"的参考信息，不该压住"正在执行/待审批/已完成"
            //   这些需要用户关注的状态。审批页**不动** —— 它需要用户按键决定，
            //   而且它本身就伴随"待审批"状态出现，收掉就没人能批了。
            if (app_screens_current() == AP_SCREEN_BALANCE) {
                ESP_LOGI(TAG, "任务状态变化，收起余额页");
                app_screens_hide(AP_SCREEN_BALANCE);
            }
        }
    }
    if (s_toast_hide_dirty) {
        s_toast_hide_dirty = false;
        // 同一轮里若还有新 toast 要显示，就以新 toast 为准（后到者胜）
        if (!s_toast_dirty) toast_hide_now();
    }
    if (s_toast_dirty) {
        s_toast_dirty = false;
        if (s_pending_toast_sticky) {
            toast_show_sticky(s_pending_toast, s_pending_toast_urgent);
        } else {
            toast_show(s_pending_toast, s_pending_toast_urgent);
        }
        screen_wake();
    }

    // 兜底：待审批等太久却没有审批页可操作 → 自行回落到空闲。
    // 宁可显示"空闲"（用户不会去操作），也不要停在一个做不到的指引上。
    if (s_state == AP_TASK_STATE_WAITING_APPROVAL && !app_screens_approval_active() &&
        s_waiting_since_ms != 0) {
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if ((uint32_t)(now - s_waiting_since_ms) >= APPROVAL_WAIT_TIMEOUT_MS) {
            ESP_LOGW(TAG, "等待授权请求超时，回落到空闲（避免卡在无效提示上）");
            s_waiting_since_ms = 0;
            app_ui_set_task_state(AP_TASK_STATE_IDLE, NULL);
        }
    }
    // 审批页一旦打开，清掉等待计时
    if (app_screens_approval_active()) s_waiting_since_ms = 0;

    // 熄屏检查。审批页打开时不熄屏 —— 那正是最需要用户操作的时候。
    // 超时来自设置页（默认**常亮** = 0，见 app_settings_logic 的档位表）。
    const int off_min = ap_settings_get()->screen_off_min;
    if (off_min > 0 && s_screen_on && !app_screens_approval_active()) {
        const uint32_t timeout_ms = (uint32_t)off_min * 60u * 1000u;
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if ((uint32_t)(now - s_last_activity_ms) >= timeout_ms) {
            screen_set(false);
        }
    }
}

// ── 构建 ──────────────────────────────────────────────────────────────────

static void build_statusbar(void)
{
    lv_obj_t *bar = lv_obj_create(s_scr);
    ui_theme_rect(bar, UI_PANEL);
    lv_obj_set_pos(bar, PX(0), PY(UI_STATUSBAR_Y));
    lv_obj_set_size(bar, UI_W, UI_STATUSBAR_H);

    // 细边框底线，清晰区分状态条与主体
    lv_obj_t *rule = lv_obj_create(s_scr);
    ui_theme_rect(rule, UI_LINE);
    lv_obj_set_pos(rule, PX(0), PY(UI_STATUSBAR_H));
    lv_obj_set_size(rule, UI_W, 1);

    // 链路信号格：4 格递增方块（放最左，视线起点）。连接状态由格数与颜色表达，
    // 未连接时全部熄灭并在主界面提示"蓝牙未连接"（见 refresh_link）。
    const int bar_x0 = UI_PAD;
    const int bar_y_base = UI_CENTER_Y(UI_STATUSBAR_H, 10) + 10;
    for (int i = 0; i < LINK_BARS; i++) {
        lv_obj_t *bar_i = lv_obj_create(bar);
        ui_theme_rect(bar_i, UI_LINE);
        lv_obj_set_pos(bar_i, bar_x0 + i * 5, bar_y_base - (4 + i * 3));
        lv_obj_set_size(bar_i, 3, 4 + i * 3);
        s_link_bars[i] = bar_i;
    }

    // 应用名使用 UI_FONT_CJK，24px 行框在 28px 高的状态条中垂直居中（y=2）
    s_app_name = ui_theme_text(bar, "AI 通行证", UI_FONT_CJK, UI_FG);
    lv_obj_set_pos(s_app_name, UI_PAD + 24, UI_CENTER_Y(UI_STATUSBAR_H, UI_LINE_H_CJK));

    // ── 电量（右上角，图标 + 百分比）──
    // 图标 = 外壳 + 正极小凸起 + 内部电量条；百分比文字右对齐排在图标左侧。
    const int batt_y = UI_CENTER_Y(UI_STATUSBAR_H, 10);
    const int shell_x = UI_W - UI_PAD - 22;

    s_batt_shell = lv_obj_create(bar);
    ui_theme_rect(s_batt_shell, UI_LINE);
    lv_obj_set_pos(s_batt_shell, shell_x, batt_y);
    lv_obj_set_size(s_batt_shell, 20, 10);

    s_batt_tip = lv_obj_create(bar);
    ui_theme_rect(s_batt_tip, UI_LINE);
    lv_obj_set_pos(s_batt_tip, shell_x + 20, batt_y + 3);
    lv_obj_set_size(s_batt_tip, 2, 4);

    s_batt_fill = lv_obj_create(bar);
    ui_theme_rect(s_batt_fill, UI_DIM);
    lv_obj_set_pos(s_batt_fill, shell_x + 2, batt_y + 2);
    lv_obj_set_size(s_batt_fill, 0, 6);

    s_batt_text = ui_theme_text(bar, "", UI_FONT_CJK, UI_DIM);
    lv_obj_set_pos(s_batt_text, shell_x - 4 - 52, UI_CENTER_Y(UI_STATUSBAR_H, UI_LINE_H_CJK));
    lv_obj_set_width(s_batt_text, 52);
    lv_obj_set_style_text_align(s_batt_text, LV_TEXT_ALIGN_RIGHT, 0);
}

static void build_body(void)
{
    // 状态色块：铺满状态条与底栏之间的整个区域，让状态占据视觉主体
    s_state_block = lv_obj_create(s_scr);
    ui_theme_rect(s_state_block, UI_DIM);
    lv_obj_set_pos(s_state_block, PX(0), PY(UI_BODY_Y));
    lv_obj_set_size(s_state_block, UI_W, UI_BODY_H);
    lv_obj_set_style_bg_opa(s_state_block, LV_OPA_20, 0);

    // 状态大字：居中放在色块偏上位置
    s_state_text = ui_theme_text(s_state_block, "空闲", UI_FONT_XL, UI_DIM);
    lv_obj_set_pos(s_state_text, 0, UI_STATE_TEXT_Y);
    lv_obj_set_width(s_state_text, UI_W);
    lv_obj_set_style_text_align(s_state_text, LV_TEXT_ALIGN_CENTER, 0);

    // 补充说明行（任务标题或状态提示），居中放在大字下方，支持换行并有充足行距。
    // ★ 高度钉死为 UI_STATE_HINT_H（3 行）+ 省略号截断：主机下发的标题长短不定，
    //   不设上限就会长文一路压到状态主体底部之外，压到分隔线和底栏上。
    s_state_hint = ui_theme_text(s_state_block, "", UI_FONT_CJK, UI_DIM);
    lv_obj_set_pos(s_state_hint, UI_PAD, UI_STATE_HINT_Y);
    lv_obj_set_width(s_state_hint, UI_W - UI_PAD * 2);
    lv_obj_set_height(s_state_hint, UI_STATE_HINT_H);
    lv_obj_set_style_text_align(s_state_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(s_state_hint, UI_TEXT_LINE_SPACE, 0);
    lv_label_set_long_mode(s_state_hint, LV_LABEL_LONG_DOT);
}

static void build_footer(void)
{
    lv_obj_t *footer = lv_obj_create(s_scr);
    ui_theme_rect(footer, UI_BG);
    lv_obj_set_pos(footer, PX(0), PY(UI_FOOTER_Y));
    lv_obj_set_size(footer, UI_W, UI_FOOTER_H);

    // 顶部加一条精致的分隔线，将状态主体与底栏自然隔开
    lv_obj_t *rule = lv_obj_create(s_scr);
    ui_theme_rect(rule, UI_LINE);
    lv_obj_set_pos(rule, PX(0), PY(UI_FOOTER_Y - 1));
    lv_obj_set_size(rule, UI_W, 1);

    // 两行独立标签：每行一个 24px 行框，行框之间留 4px（中文 20px 的实际字形
    // 之间约有 6px 视觉间隙）。上一版两行行框首尾相接，只留 2px 字缝，用户看到
    // 的就是"字都挤在一起" —— 行距必须从字体行高推出，不能拍脑袋写 y。
    s_footer_l1 = ui_theme_text(footer, "", UI_FONT_CJK16, UI_DIM);
    lv_obj_set_pos(s_footer_l1, 0, UI_FOOTER_LINE1_Y);
    lv_obj_set_width(s_footer_l1, UI_W);
    lv_obj_set_style_text_align(s_footer_l1, LV_TEXT_ALIGN_CENTER, 0);

    s_footer_l2 = ui_theme_text(footer, "", UI_FONT_CJK16, UI_DIM);
    lv_obj_set_pos(s_footer_l2, 0, UI_FOOTER_LINE2_Y);
    lv_obj_set_width(s_footer_l2, UI_W);
    lv_obj_set_style_text_align(s_footer_l2, LV_TEXT_ALIGN_CENTER, 0);
}

static void build_toast(void)
{
    // 提示条浮在状态主体之上，用于"已审批""连接已断开"这类短暂通知
    s_toast = lv_obj_create(s_scr);
    ui_theme_rect(s_toast, UI_PANEL);
    lv_obj_set_pos(s_toast, PX(UI_PAD), PY(UI_BODY_Y + 16));
    lv_obj_set_size(s_toast, UI_W - UI_PAD * 2, UI_TOAST_H);
    // 只留左边框作为强调（比四周描边更轻），颜色表示紧急程度
    lv_obj_set_style_border_side(s_toast, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(s_toast, 4, 0);
    lv_obj_set_style_border_color(s_toast, lv_color_hex(UI_ACCENT), 0);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);

    // 文本限 2 行、超出省略号：提示文案最长 80 字节，不限行会撑破提示条底边
    s_toast_text = ui_theme_text(s_toast, "", UI_FONT_CJK, UI_FG);
    lv_obj_set_pos(s_toast_text, 10, UI_TOAST_TEXT_Y);
    lv_obj_set_width(s_toast_text, UI_W - UI_PAD * 2 - 20);
    lv_obj_set_height(s_toast_text, UI_TOAST_TEXT_H);
    lv_obj_set_style_text_line_space(s_toast_text, UI_TEXT_LINE_SPACE, 0);
    lv_label_set_long_mode(s_toast_text, LV_LABEL_LONG_DOT);
}

void app_ui_create(void)
{
    if (s_scr) return;

    s_scr = lv_obj_create(NULL);
    ui_theme_rect(s_scr, UI_BG);

    build_statusbar();
    build_body();
    build_footer();
    build_toast();

    lv_screen_load(s_scr);

    // 覆盖层（审批/余额）在主页之后创建，靠 HIDDEN 标志盖在主页上
    app_screens_create();

    s_timer = lv_timer_create(ui_tick, 20, NULL);

    s_pending_link = AP_LINK_OFF;
    refresh_link();
    refresh_state();
    bsp_display_backlight((uint8_t)ap_settings_get()->brightness);
    note_activity();

    ESP_LOGI(TAG, "界面已创建（精简状态版）");
}

void app_ui_destroy(void)
{
    // 顺序要紧：先覆盖层与定时器，再主页。定时器回调会访问覆盖层对象，
    // 反过来销毁会留下悬空指针。
    app_screens_destroy();
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_toast_timer) { lv_timer_delete(s_toast_timer); s_toast_timer = NULL; }
    if (s_scr) { lv_obj_delete(s_scr); s_scr = NULL; }

    s_link_bars[0] = NULL; s_link_bars[1] = NULL;
    s_link_bars[2] = NULL; s_link_bars[3] = NULL;
    s_batt_shell = NULL; s_batt_fill = NULL; s_batt_tip = NULL; s_batt_text = NULL;
    s_app_name = NULL; s_state_block = NULL;
    s_state_text = NULL; s_state_hint = NULL; s_footer_l1 = NULL; s_footer_l2 = NULL;
    s_toast = NULL; s_toast_text = NULL;
}

// ── 跨任务更新入口 ────────────────────────────────────────────────────────

void app_ui_on_link_state(ap_link_state_t state)
{
    s_pending_link = state;
    s_link_dirty = true;
}

void app_ui_apply_brightness(void)
{
    // 屏亮着才调背光：熄屏状态下改亮度不该把屏点亮
    bsp_display_backlight(s_screen_on ? (uint8_t)ap_settings_get()->brightness : 0);
}

void app_ui_set_task_state(ap_task_state_t state, const char *title)
{
    // ★ 录音期间**缓存而不应用**：主机的 task.state 推送可能因为链路重连、
    //   定时器等任何原因在录音中途到达，覆盖"识别中…"的显示 ——
    //   表现为"按住说话到一半界面跳到空闲"（真机返工记录）。
    //   这里把状态暂存起来，等 app_ui_set_recording(false) 时一次性应用。
    if (app_voice_recording()) {
        s_deferred_state = state;
        clean_text(title, s_deferred_title, sizeof(s_deferred_title));
        s_deferred_state_pending = true;
        return;
    }
    s_pending_state = state;
    clean_text(title, s_pending_title, sizeof(s_pending_title));
    s_state_dirty = true;
}

/**
 * 录音状态变化。由 app_voice.c 在开始/结束录音时调用。
 *
 * 结束录音时把缓存的状态变化一次性应用（如果有）。
 */
void app_ui_set_recording(bool recording)
{
    if (recording) return;   // 开始录音不需要额外动作
    if (!s_deferred_state_pending) return;
    s_deferred_state_pending = false;
    s_pending_state = s_deferred_state;
    snprintf(s_pending_title, sizeof(s_pending_title), "%s", s_deferred_title);
    s_state_dirty = true;
}

void app_ui_set_device_info(int battery_percent, int8_t rssi, uint16_t mtu)
{
    // 电量放右上角显示（用户要求）；RSSI 由信号格表达（refresh_signal_bars），
    // MTU 是链路细节，只进控制面板。
    (void)rssi;
    (void)mtu;
    s_pending_battery = battery_percent;   // -1 = 未知
    s_batt_dirty = true;
}

void app_ui_post_toast(const char *text, bool urgent)
{
    if (!text) return;
    // ★ UTF-8 安全截断（而不是 snprintf 按字节硬切）：识别结果是动态长句，
    //   按字节切会切在汉字中间，LVGL 渲染出方块。见 utf8_clamp.c 的说明。
    utf8_copy_clamped(text, s_pending_toast, sizeof(s_pending_toast));
    s_pending_toast_urgent = urgent;
    s_pending_toast_sticky = false;
    s_toast_dirty = true;
}

/** 粘性 toast：不自动消失，直到下一个 toast 或 app_ui_hide_toast()。 */
void app_ui_post_toast_sticky(const char *text, bool urgent)
{
    if (!text) return;
    utf8_copy_clamped(text, s_pending_toast, sizeof(s_pending_toast));
    s_pending_toast_urgent = urgent;
    s_pending_toast_sticky = true;
    s_toast_dirty = true;
}

// ── 按键 ──────────────────────────────────────────────────────────────────

bool app_ui_handle_key(int btn, int event)
{
    if (!s_scr) return false;

    // 任何按键都算"有动静"，推迟熄屏并确保屏幕可见
    screen_wake();

    // 覆盖层优先消费按键（审批页尤其不能漏给主页）
    if (app_screens_handle_key(btn, event)) return true;

    // PTT 录音：确定键的按下/松开交给 app_main（按住说话）。
    // ★ 这两个事件必须放行 —— 曾被下面的兜底消费拦住，app_main 的录音分支
    //   永远收不到，表现就是"按了没反应"（真实反馈过的缺陷）。
    if (btn == APP_UI_BTN_OK && (event == APP_UI_EV_PRESS || event == APP_UI_EV_RELEASE)) return false;

    // 下键长按交给 app_main（请求余额）
    if (btn == APP_UI_BTN_DOWN && event == APP_UI_EV_LONG) return false;

    // 其余按键在精简版里没有动作，但**消费掉**，避免漏出去触发别的行为
    return true;
}
