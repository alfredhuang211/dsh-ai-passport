// main/app_screens.c —— 覆盖层：审批 / 余额（精简版）
//
// ══════════════════════════════════════════════════════════════════════════
//  相比上一版删掉了什么
// ══════════════════════════════════════════════════════════════════════════
//  · **任务详情页** —— 设备不再显示任务列表，也就没有"点进去看某一条"的入口。
//    任务内容在主机上看远比 240×320 上舒服。
//  · 保留**审批**（需要按键交互）与**余额**（一次性查询，看完即走）。
//
// ══════════════════════════════════════════════════════════════════════════
//  审批页的设计（这是整个界面里唯一有实际风险的操作）
// ══════════════════════════════════════════════════════════════════════════
//  三个选项，语义与 DSH 的审批确认一一对应：
//
//      运行一次    decision=allow, scope=once     ← 默认停留在这里
//      拒绝        decision=deny,  scope=once
//
//  ★ 这里**刻意没有**"总是运行"。原因是 DSH 的审批授权按设计只对单次操作有效：
//        ApprovalOutcome = allowed-once | rejected | cancelled | unavailable
//    其文档写明 "grants apply only to the requested action"，没有"记住这个工具的授权"。
//    提供一个做不到的选项，与"提示只能写按了确实会发生的事"是同一条原则 ——
//    用户选了"总是运行"却发现下次还被问，比没有这个选项更让人困惑。
//
//  ★ 拒绝只需一次确认，允许类需要二次确认。
//    理由是**误操作的代价不对称**：误拒只是重来一次，误允许可能写坏文件。
//
//  ★ 倒计时超时按"拒绝"上报，绝不自动放行。
//    超时视为"没看到"；默默同意是最危险的默认行为。
#include "app_screens.h"

#include "app_alert.h"
#include "app_pair.h"
#include "app_settings.h"
#include "app_link.h"
#include "app_proto.h"
#include "app_question_logic.h"
#include "app_session.h"
#include "app_session_logic.h"
#include "app_ui.h"
#include "app_ui_theme.h"
#include "app_voice.h"
#include "utf8_clamp.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lvgl.h"

static const char *TAG = "app_screens";

#define APPROVAL_ID_MAX 40

// 覆盖层标题行与页脚、审批页/余额页的全部坐标规格见 app_ui_theme.h（OV_*）。
// 审批选项个数
#define APPR_OPTIONS 2

// ── 待处理投递（BLE 回调只写结构，LVGL 定时器消费）────────────────────────

typedef struct {
    char request_id[APPROVAL_ID_MAX];
    // ★ 只有一行极短摘要：完整工具名/原因在电脑卡片上看（用户决策 2026-10-06）
    char summary[64];
    // 协议 v2：哪个会话在等审批（弹窗第二行，只显示会话名）。
    char session_title[AP_SESSION_TITLE_MAX];
    bool high_risk;
    uint32_t timeout_ms;
} pending_approval_t;

typedef struct {
    char total[24];
    char currency[8];
    char recharge[24];
    char bonus[24];
    char today_used[24];
} pending_balance_t;

// ── 界面对象 ──────────────────────────────────────────────────────────────

static lv_obj_t *s_balance;
static lv_obj_t *s_balance_total;
static lv_obj_t *s_balance_lines;

static lv_obj_t *s_approval;
static lv_obj_t *s_appr_tool;
static lv_obj_t *s_appr_option[APPR_OPTIONS];
static lv_obj_t *s_appr_opt_bar[APPR_OPTIONS];
static lv_obj_t *s_appr_opt_text[APPR_OPTIONS];
static lv_obj_t *s_appr_hint;

static ap_screen_t s_current = AP_SCREEN_NONE;
static ap_screen_action_t s_action = AP_SCREEN_ACTION_NONE;

// 审批状态
static char s_appr_id[APPROVAL_ID_MAX];
static int s_appr_selected = 0;
static bool s_appr_submitted = false;

// 投递缓冲与脏标志
static volatile bool s_appr_dirty = false;
static pending_approval_t s_post_appr;
static volatile bool s_appr_done_dirty = false;
static char s_appr_done_id[APPROVAL_ID_MAX];
static volatile bool s_balance_dirty = false;
static pending_balance_t s_post_balance;
// ★ 余额页**只在用户按了下键**之后才弹出。主机在连上/定时刷新时会主动推余额
//   （balance.refresh 默认 push），若收到就弹，设备会在连接完成时自己跳到
//   余额页（真机返工记录）。收到数据只更新缓存，不抢屏幕。
static volatile bool s_balance_show_pending = false;
static char s_tx_buf[256];

// ── 多会话（协议 v2）：列表页 + 会话页 ────────────────────────────────────
//
// 视觉原则（用户要求）：状态尽量用颜色/圆点表达，关键文字只留会话名。
// 行布局：左侧 4px 状态色条 + 标题（16px）+ 右侧 8px 未读圆点。
// 列表最多 AP_SESSION_MAX 行，8×(26+2) 正好落在标题栏与页脚之间。

#define SESS_ROW_H 26
#define SESS_ROW_GAP 2
#define SESS_ROWS_Y (OV_TITLE_H + 8)
#define SESS_DOT_SIZE 8

static lv_obj_t *s_sessions_panel;
static lv_obj_t *s_sess_hint;
static lv_obj_t *s_sess_rows[AP_SESSION_MAX];
static lv_obj_t *s_sess_bar[AP_SESSION_MAX];
static lv_obj_t *s_sess_text[AP_SESSION_MAX];
static lv_obj_t *s_sess_dot[AP_SESSION_MAX];

static ap_session_row_t s_sess_view[AP_SESSION_MAX];
static int s_sess_count = 0;
static int s_sess_cursor = 0;
static char s_sess_selected[AP_SESSION_HANDLE_MAX];
static char s_sess_hash[AP_SESSION_MAX][AP_SESSION_TITLE_MAX];
static uint8_t s_sess_flags[AP_SESSION_MAX];   // bit0 可见 bit1 选中 bit2 未读
static bool s_sess_cache_valid = false;

static volatile bool s_sess_dirty = false;
static ap_session_row_t s_post_sess[AP_SESSION_MAX];
static int s_post_sess_count = 0;
static char s_post_sess_sel[AP_SESSION_HANDLE_MAX];

// 会话页
static lv_obj_t *s_session_panel;
static lv_obj_t *s_session_hint;
static lv_obj_t *s_session_head;
static lv_obj_t *s_session_state;
static lv_obj_t *s_session_meta;
static lv_obj_t *s_session_summary;
static char s_session_handle[AP_SESSION_HANDLE_MAX];
static ap_session_detail_t s_session_detail_view;
static volatile bool s_detail_dirty = false;
static ap_session_detail_t s_post_detail;

// 审批页「会话：xxx」行
static lv_obj_t *s_appr_session;

// ── 识别结果卡（docs/06 §2）────────────────────────────────────────────────
//
// 语音识别结果不再只弹 toast（toast 无交互、且会被提示队列挤掉 —— 05 §9 缺陷 A），
// 而是弹一张等按键的结果卡：
//     单击确定 = 填入电脑输入框      双击确定 = 整段替换并发送
//     按住确定 = 放弃这条结果，直接进入"按住说话"重录
// 设备只上报意图（voice.action），真正动输入框的是主机侧挂件。

#define VOICE_ID_MAX 48
#define VOICE_TEXT_MAX 512
// 结果卡 30 秒无操作自动收起；文本保留在主机语音列表，可去挂件里手动填入。
#define VOICE_CARD_TTL_MS 30000

typedef struct {
    char result_id[VOICE_ID_MAX];
    char text[VOICE_TEXT_MAX];
    // 协议 v2：录音时所在的会话 handle（voice.action 带回去，避免发错会话）。
    char target[AP_SESSION_HANDLE_MAX];
} pending_voice_t;

static lv_obj_t *s_voice;
static lv_obj_t *s_voice_text;
static lv_obj_t *s_voice_box;    // 正文滚动容器（限制 WRAP 标签不乱长）
static lv_obj_t *s_voice_hint;
static lv_obj_t *s_voice_foot_rule;   // 结果卡不用共用底栏，这条分隔线要收掉

// ── 系统设置页（阶段 B）──
static lv_obj_t *s_settings;
static lv_obj_t *s_settings_hint;
static lv_obj_t *s_set_rows[AP_SETTINGS_ITEM_COUNT];
static lv_obj_t *s_set_bar[AP_SETTINGS_ITEM_COUNT];
static lv_obj_t *s_set_text[AP_SETTINGS_ITEM_COUNT];
static int s_set_cursor = 0;
// 设置页最后一行是「配对」（不是普通档位项，单独处理）
static lv_obj_t *s_set_pair_row;
static lv_obj_t *s_set_pair_bar;
static lv_obj_t *s_set_pair_text;

// ── 配对码页（未配对的主机接入时弹出）──
static lv_obj_t *s_pair;
static lv_obj_t *s_pair_code_text;
static lv_obj_t *s_pair_sub_text;
static volatile bool s_pair_dirty = false;
static char s_post_pair_code[16];
static char s_post_pair_id[16];
static lv_obj_t *s_voice_keys;   // 结果卡专属的第二行按键提示（底栏只有一行，放不下四条）
static char s_voice_id[VOICE_ID_MAX];
static char s_voice_target[AP_SESSION_HANDLE_MAX];   // 本次结果对应的会话（voice.action 回传）
static bool s_voice_sent = false;
static uint32_t s_voice_deadline_ms = 0;
// 结果卡上的文本是否已被用户清除（清除后禁止再填入/发送）
static volatile bool s_voice_dirty = false;
static pending_voice_t s_post_voice;

// ── 追问 / 计划评审（docs/06 §3/§4）────────────────────────────────────────
//
// 主机持有整批真相，设备一次只显示一题：勾选变化即时回发 question.pick，
// 主机累计；提交/取消/要求修改走 question.answer 三个动作值。
// 行列表 = 选项行 + custom 行 + 动作行（下一题/提交 + 跳过）；
// 计划评审复用同一块面板，行固定为（要求修改 / 批准计划）。

#define Q_ROW_MAX (AP_Q_MAX_OPTIONS + 3)   // 5 选项 + custom + 2 动作
#define Q_CUSTOM_MAX 200

typedef struct {
    char call_id[AP_QUESTION_ID_MAX];
    bool ok;
    char reason[48];
} pending_q_done_t;

static lv_obj_t *s_question;
static lv_obj_t *s_q_text;
static lv_obj_t *s_qvoice;
static lv_obj_t *s_qvoice_text;
static lv_obj_t *s_qvoice_hint;
static lv_obj_t *s_qvoice_title;
static lv_obj_t *s_q_rows[Q_ROW_MAX];
static lv_obj_t *s_q_row_bar[Q_ROW_MAX];
static lv_obj_t *s_q_row_text[Q_ROW_MAX];
static lv_obj_t *s_q_hint;
static lv_obj_t *s_q_title;

static ap_question_msg_t s_q;
static char s_q_labels[AP_Q_MAX_OPTIONS][AP_Q_LABEL_SIZE];
static bool s_q_selected[AP_Q_MAX_OPTIONS];
static int s_q_option_count = 0;
static int s_q_cursor = 0;
static volatile bool s_q_voice_mode = false;   // 语音输入页是否在前台（跨任务读写）
// ★ 识别结果由 app_task（链路消息队列）投递到这里，**不能在那里碰 LVGL**。
//   曾经直接在回调里 show_panel/refresh_*，导致 LVGL 内部刷新区链表被并发破坏，
//   真机现象是整机冻死（WDT 反复报 IDLE 饿死、PC 落在 lv_inv_area 死循环）。
static volatile bool s_q_voice_result_dirty = false;
static char s_q_voice_result_text[512];
static char s_q_custom[Q_CUSTOM_MAX];

static volatile bool s_q_dirty = false;
static ap_question_msg_t s_post_q;
static volatile bool s_q_done_dirty = false;
static pending_q_done_t s_post_q_done;

// ── 增量刷新的"已渲染状态"缓存 ────────────────────────────────────────────
//
// lv_label_set_text 会复制文本并让整行失效重绘；逐行无条件重写在 C3 上是
// 可感知的卡顿来源（一次光标移动 = 整列表 8 行中文全部重绘 + 多趟 SPI 刷屏）。
// 这里记住每行最近一次**真正写进 LVGL** 的文本与高亮状态，新旧一致就跳过；
// 行文本本身由 app_question_logic 的纯逻辑产出（host 测试钉死）。
static char s_q_row_cache[Q_ROW_MAX][AP_Q_ROW_TEXT_MAX];
static uint16_t s_q_row_state[Q_ROW_MAX];   // bit0=可见 bit1=光标高亮
static bool s_q_row_cache_valid;
static int s_appr_sel_cached = -1;          // 审批页已渲染的选中项（-1=未渲染）

// ── 选项表（文案 / 决策 / 范围 / 颜色）────────────────────────────────────

typedef struct {
    const char *label;      // 屏幕上的文案
    const char *decision;   // AP_DECISION_*
    const char *scope;      // AP_SCOPE_*
    uint32_t color;         // 选中时的强调色
} approval_option_t;

// ★ 顺序即上/下键顺序，且**第 0 项是默认停留项**。
//   用户要求默认停在「运行一次」—— 大多数审批确实只该放行这一次。
static const approval_option_t s_options[APPR_OPTIONS] = {
    { "允许一次", AP_DECISION_ALLOW, AP_SCOPE_ONCE, UI_ACCENT },
    { "拒绝",     AP_DECISION_DENY,  AP_SCOPE_ONCE, UI_ERR    },
};

// ── 小工具 ────────────────────────────────────────────────────────────────

static void clean(const char *src, char *dst, size_t dst_size)
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

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static lv_obj_t *hrule(lv_obj_t *parent, int y)
{
    lv_obj_t *line = lv_obj_create(parent);
    ui_theme_rect(line, UI_LINE);
    lv_obj_set_pos(line, 0, y);
    lv_obj_set_size(line, UI_W, 1);
    return line;
}

static lv_obj_t *make_overlay(const char *title, lv_obj_t **out_hint, lv_obj_t **out_title,
                              lv_obj_t **out_rule)
{
    lv_obj_t *panel = lv_obj_create(lv_screen_active());
    ui_theme_rect(panel, UI_BG);
    lv_obj_set_pos(panel, PX(0), PY(0));
    lv_obj_set_size(panel, UI_W, UI_H);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);

    // 标题使用 UI_FONT_CJK，24px 行框在 28px 顶栏中垂直居中（y=2）
    lv_obj_t *head = ui_theme_text(panel, title, UI_FONT_CJK, UI_ACCENT);
    lv_obj_set_pos(head, UI_PAD, UI_CENTER_Y(OV_TITLE_H, UI_LINE_H_CJK));
    hrule(panel, OV_TITLE_H);

    // 底栏提示同样采用 UI_FONT_CJK，24px 行框在 24px 页脚中垂直居中
    // 底栏提示用 16px 中文：一行约 13 个汉字（20px 只有 10 个，长提示会被截断）
    lv_obj_t *hint = ui_theme_text(panel, "确定键返回", UI_FONT_CJK16, UI_DIM);
    lv_obj_set_pos(hint, UI_PAD, UI_H - OV_FOOT_H + UI_CENTER_Y(OV_FOOT_H, UI_LINE_H_CJK16));
    lv_obj_set_width(hint, UI_W - UI_PAD * 2);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *rule = hrule(panel, UI_H - OV_FOOT_H);

    if (out_rule) *out_rule = rule;
    if (out_hint) *out_hint = hint;
    if (out_title) *out_title = head;
    return panel;
}

// ── 创建 ──────────────────────────────────────────────────────────────────

void app_screens_create(void)
{
    if (s_balance) return;

    // 行缓存记的是旧 LVGL 对象的渲染态：重建后必须失效，否则增量刷新会
    // 跳过真正需要写入的新标签（新屏幕上出现空白行）。
    s_q_row_cache_valid = false;
    s_appr_sel_cached = -1;

    // ══ 余额 ══
    // 总额是这一页唯一的主角，给强调色与中文 20px
    lv_obj_t *bh = NULL;
    s_balance = make_overlay("DeepSeek 余额", &bh, NULL, NULL);

    s_balance_total = ui_theme_text(s_balance, "--", UI_FONT_CJK, UI_ACCENT);
    lv_obj_set_pos(s_balance_total, UI_PAD, OV_BAL_TOTAL_Y);
    lv_obj_set_width(s_balance_total, UI_W - UI_PAD * 2);

    // 明细行采用中文 20px，行间距 8px（行距 32px），多行数字明细舒展不挤；
    // 行数钉死为 OV_BAL_LINES，超出省略号，不允许长明细压到页脚
    s_balance_lines = ui_theme_text(s_balance, "", UI_FONT_CJK, UI_DIM);
    lv_obj_set_pos(s_balance_lines, UI_PAD, OV_BAL_LINES_Y);
    lv_obj_set_width(s_balance_lines, UI_W - UI_PAD * 2);
    lv_obj_set_height(s_balance_lines, OV_BAL_LINES_H);
    lv_obj_set_style_text_line_space(s_balance_lines, OV_BAL_LINE_SPACE, 0);
    lv_label_set_long_mode(s_balance_lines, LV_LABEL_LONG_DOT);

    // ══ 识别结果卡 ══
    // 文本区给固定高度 + 自动换行 + 纵向滚动：识别文本长短不定，
    // 截断长句会让人在没看全的情况下按下"发送"。
    s_voice = make_overlay("识别结果", &s_voice_hint, NULL, &s_voice_foot_rule);

    // 结果卡不用共用底栏：三行按键说明取代了它，错误反馈走 toast。
    // 把底栏分隔线与底栏文字都收掉 —— 否则三行说明下面会留一节空白（用户指出）——
    // 省出来的空间交给正文与说明区。
    if (s_voice_foot_rule) lv_obj_add_flag(s_voice_foot_rule, LV_OBJ_FLAG_HIDDEN);
    if (s_voice_hint) lv_obj_add_flag(s_voice_hint, LV_OBJ_FLAG_HIDDEN);

    // ★ 正文放进**滚动容器**（与追问页行列表同款写法）。真机反馈："按键提示跑到内容框里" ——
    //   根因是 LVGL 的 WRAP 标签会随文本**自动长高**，直接给标签设高度根本限制不住它，
    //   长文本就一路压到下面三行提示上。容器把正文裁在自己的区域里，滚动也落在容器上。
    s_voice_box = lv_obj_create(s_voice);
    ui_theme_rect(s_voice_box, UI_BG);
    lv_obj_set_pos(s_voice_box, 0, OV_VOICE_TEXT_Y);
    lv_obj_set_size(s_voice_box, UI_W, OV_VOICE_TEXT_H);
    lv_obj_set_scroll_dir(s_voice_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_voice_box, LV_SCROLLBAR_MODE_AUTO);

    s_voice_text = ui_theme_text(s_voice_box, "", UI_FONT_CJK, UI_FG);
    lv_obj_set_pos(s_voice_text, UI_PAD, 0);
    lv_obj_set_width(s_voice_text, UI_W - UI_PAD * 2 - 6);   // 6px 留给滚动条
    lv_obj_set_height(s_voice_text, LV_SIZE_CONTENT);        // 高度随文本，由容器裁剪
    lv_obj_set_style_text_line_space(s_voice_text, UI_TEXT_LINE_SPACE, 0);
    lv_label_set_long_mode(s_voice_text, LV_LABEL_LONG_WRAP);

    // 正文与按键说明之间画一条分隔线，视觉上明确"这两块不是一回事"
    hrule(s_voice, OV_VOICE_KEYS_Y - 6);

    // 底栏只有 208px（≈10 个汉字），装不下三条按键说明；这里给结果卡单独留三行
    // （用户指定文案），并把正文区高度相应减掉 OV_VOICE_KEYS_H。
    s_voice_keys = ui_theme_text(s_voice,
                                 "单击上键 删除/返回\n单击确认 填入\n长按确认 发送",
                                 UI_FONT_CJK16, UI_DIM);
    lv_obj_set_pos(s_voice_keys, UI_PAD, OV_VOICE_KEYS_Y);
    lv_obj_set_size(s_voice_keys, UI_W - UI_PAD * 2, OV_VOICE_KEYS_H);
    lv_obj_set_style_text_align(s_voice_keys, LV_TEXT_ALIGN_CENTER, 0);

    // ══ 追问 / 计划评审 ══
    // 题干区固定 3 行（超出省略号）；行列表放进滚动容器 ——
    // 选项+custom+动作最多 8 行，超出可见区的行必须能滚到，否则光标走到
    // 看不见的行上，用户会以为"按键又没反应"（项目里踩过的真坑）。
    s_question = make_overlay("问题", &s_q_hint, &s_q_title, NULL);

    s_q_text = ui_theme_text(s_question, "", UI_FONT_CJK, UI_FG);
    lv_obj_set_pos(s_q_text, UI_PAD, OV_Q_TEXT_Y);
    lv_obj_set_size(s_q_text, UI_W - UI_PAD * 2, OV_Q_TEXT_H);
    lv_obj_set_style_text_line_space(s_q_text, UI_TEXT_LINE_SPACE, 0);
    lv_label_set_long_mode(s_q_text, LV_LABEL_LONG_DOT);

    lv_obj_t *q_list = lv_obj_create(s_question);
    ui_theme_rect(q_list, UI_BG);
    lv_obj_set_pos(q_list, 0, OV_Q_LIST_Y);
    lv_obj_set_size(q_list, UI_W, OV_Q_LIST_H);
    lv_obj_set_scroll_dir(q_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(q_list, LV_SCROLLBAR_MODE_AUTO);

    for (int i = 0; i < Q_ROW_MAX; i++) {
        const int y = i * (OV_Q_ROW_H + OV_Q_ROW_GAP);
        lv_obj_t *row = lv_obj_create(q_list);
        ui_theme_rect(row, UI_BG);
        lv_obj_set_pos(row, UI_PAD, y);
        lv_obj_set_size(row, UI_W - UI_PAD * 2, OV_Q_ROW_H);

        s_q_row_bar[i] = lv_obj_create(row);
        ui_theme_rect(s_q_row_bar[i], UI_DIM);
        lv_obj_set_pos(s_q_row_bar[i], 0, 0);
        lv_obj_set_size(s_q_row_bar[i], 4, OV_Q_ROW_H);

        s_q_row_text[i] = ui_theme_text(row, "", UI_FONT_CJK, UI_DIM);
        lv_obj_set_pos(s_q_row_text[i], 12, UI_CENTER_Y(OV_Q_ROW_H, UI_LINE_H_CJK));
        s_q_rows[i] = row;
    }

    // ══ 语音输入页（自定义答案：独立一页，可放弃返回选择界面）══
    s_qvoice = make_overlay("语音输入", &s_qvoice_hint, &s_qvoice_title, NULL);

    s_qvoice_text = ui_theme_text(s_qvoice, "按住确定说话", UI_FONT_CJK, UI_FG);
    lv_obj_set_pos(s_qvoice_text, 0, OV_QV_TEXT_Y);
    lv_obj_set_size(s_qvoice_text, UI_W, OV_QV_TEXT_H);
    lv_obj_set_style_text_align(s_qvoice_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(s_qvoice_text, UI_TEXT_LINE_SPACE, 0);
    lv_label_set_long_mode(s_qvoice_text, LV_LABEL_LONG_WRAP);

    // ══ 审批 ══
    s_approval = make_overlay("等待审批", &s_appr_hint, NULL, NULL);

    // 工具名回答"要动什么"，给 20px 警告色 —— 本页最该被看清的信息
    s_appr_tool = ui_theme_text(s_approval, "", UI_FONT_CJK, UI_WARN);
    lv_obj_set_pos(s_appr_tool, UI_PAD, OV_APPR_TOOL_Y);
    lv_obj_set_width(s_appr_tool, UI_W - UI_PAD * 2);
    lv_label_set_long_mode(s_appr_tool, LV_LABEL_LONG_DOT);

    // 协议 v2：第二行只显示「会话：<会话名>」，其余正文仍在电脑卡片上。
    s_appr_session = ui_theme_text(s_approval, "", UI_FONT_CJK16, UI_DIM);
    lv_obj_set_pos(s_appr_session, UI_PAD, OV_APPR_TOOL_Y + UI_LINE_H_CJK + 2);
    lv_obj_set_width(s_appr_session, UI_W - UI_PAD * 2);
    lv_label_set_long_mode(s_appr_session, LV_LABEL_LONG_DOT);
    lv_obj_add_flag(s_appr_session, LV_OBJ_FLAG_HIDDEN);

    // 审批原因：最多 3 行（超出省略号）。★ 不限行会出真问题：原因最长 95 字，
    // 折 10 行都能排得下，会一路盖住下方的倒计时条和选项 —— 文字叠文字。
    // 倒计时：条 + 秒数，放在选项上方
    // 倒计时文本使用 UI_FONT_CJK

    // 两个选项：左侧色条 + 文字
    for (int i = 0; i < APPR_OPTIONS; i++) {
        const int y = OV_APPR_OPTS_Y + i * (OV_APPR_OPT_H + OV_APPR_OPT_GAP);
        lv_obj_t *opt = lv_obj_create(s_approval);
        ui_theme_rect(opt, UI_BG);
        lv_obj_set_pos(opt, UI_PAD, y);
        lv_obj_set_size(opt, UI_W - UI_PAD * 2, OV_APPR_OPT_H);

        s_appr_opt_bar[i] = lv_obj_create(opt);
        ui_theme_rect(s_appr_opt_bar[i], UI_DIM);
        lv_obj_set_pos(s_appr_opt_bar[i], 0, 0);
        lv_obj_set_size(s_appr_opt_bar[i], 4, OV_APPR_OPT_H);

        s_appr_opt_text[i] = ui_theme_text(opt, s_options[i].label, UI_FONT_CJK, UI_DIM);
        lv_obj_set_pos(s_appr_opt_text[i], 12, UI_CENTER_Y(OV_APPR_OPT_H, UI_LINE_H_CJK));

        s_appr_option[i] = opt;
    }

    // ══ 系统设置 ══
    s_settings = make_overlay("系统设置", &s_settings_hint, NULL, NULL);
    if (s_settings_hint) lv_label_set_text(s_settings_hint, "上/下选择 单击切换 长按退出");
    for (int i = 0; i < AP_SETTINGS_ITEM_COUNT; i++) {
        const int y = OV_SET_ROWS_Y + i * (OV_Q_ROW_H + OV_Q_ROW_GAP);
        lv_obj_t *row = lv_obj_create(s_settings);
        ui_theme_rect(row, UI_BG);
        lv_obj_set_pos(row, UI_PAD, y);
        lv_obj_set_size(row, UI_W - UI_PAD * 2, OV_Q_ROW_H);
        s_set_rows[i] = row;

        s_set_bar[i] = lv_obj_create(row);
        ui_theme_rect(s_set_bar[i], UI_LINE);
        lv_obj_set_pos(s_set_bar[i], 0, 0);
        lv_obj_set_size(s_set_bar[i], 4, OV_Q_ROW_H);

        s_set_text[i] = ui_theme_text(row, "", UI_FONT_CJK, UI_FG);
        lv_obj_set_pos(s_set_text[i], 12, UI_CENTER_Y(OV_Q_ROW_H, UI_LINE_H_CJK));
        lv_obj_set_width(s_set_text[i], UI_W - UI_PAD * 2 - 20);
        lv_label_set_long_mode(s_set_text[i], LV_LABEL_LONG_DOT);
    }

    // 设置页最后一行：配对状态（未配对时单击确定弹出配对码页）。
    // ★ 两行高（OV_SET_PAIR_H）：一行放不下"已配对（主机名）"，会被省略号
    //   截断（真机反馈"已配对没显示全"）。状态行 + 主机名行各占一行，
    //   定高 + LONG_DOT：只有超长主机名才落省略号。
    {
        const int y = OV_SET_ROWS_Y + AP_SETTINGS_ITEM_COUNT * (OV_Q_ROW_H + OV_Q_ROW_GAP);
        lv_obj_t *row = lv_obj_create(s_settings);
        ui_theme_rect(row, UI_BG);
        lv_obj_set_pos(row, UI_PAD, y);
        lv_obj_set_size(row, UI_W - UI_PAD * 2, OV_SET_PAIR_H);
        s_set_pair_row = row;

        s_set_pair_bar = lv_obj_create(row);
        ui_theme_rect(s_set_pair_bar, UI_LINE);
        lv_obj_set_pos(s_set_pair_bar, 0, 0);
        lv_obj_set_size(s_set_pair_bar, 4, OV_SET_PAIR_H);

        s_set_pair_text = ui_theme_text(row, "", UI_FONT_CJK, UI_FG);
        lv_obj_set_pos(s_set_pair_text, 12, 0);
        lv_obj_set_size(s_set_pair_text, UI_W - UI_PAD * 2 - 20, OV_SET_PAIR_H);
        lv_label_set_long_mode(s_set_pair_text, LV_LABEL_LONG_DOT);
    }

    // ══ 配对码页 ══
    s_pair = make_overlay("配对", &s_pair_sub_text, NULL, NULL);
    if (s_pair_sub_text) lv_label_set_text(s_pair_sub_text, "长按确定关闭");
    s_pair_code_text = ui_theme_text(s_pair, "", UI_FONT_CJK, UI_ACCENT);
    lv_obj_set_pos(s_pair_code_text, UI_PAD, OV_SET_ROWS_Y + 20);
    lv_obj_set_width(s_pair_code_text, UI_W - UI_PAD * 2);
    lv_obj_set_style_text_align(s_pair_code_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(s_pair_code_text, UI_TEXT_LINE_SPACE, 0);

    // ══ 会话列表（协议 v2 主页）══
    s_sessions_panel = make_overlay("会话", &s_sess_hint, NULL, NULL);
    if (s_sess_hint) lv_label_set_text(s_sess_hint, "上/下选择 确定进入");
    for (int i = 0; i < AP_SESSION_MAX; i++) {
        const int y = SESS_ROWS_Y + i * (SESS_ROW_H + SESS_ROW_GAP);
        lv_obj_t *row = lv_obj_create(s_sessions_panel);
        ui_theme_rect(row, UI_BG);
        lv_obj_set_pos(row, UI_PAD, y);
        lv_obj_set_size(row, UI_W - UI_PAD * 2, SESS_ROW_H);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        s_sess_rows[i] = row;

        s_sess_bar[i] = lv_obj_create(row);
        ui_theme_rect(s_sess_bar[i], UI_DIM);
        lv_obj_set_pos(s_sess_bar[i], 0, 0);
        lv_obj_set_size(s_sess_bar[i], 4, SESS_ROW_H);

        s_sess_text[i] = ui_theme_text(row, "", UI_FONT_CJK16, UI_FG_DIM);
        lv_obj_set_pos(s_sess_text[i], 12, UI_CENTER_Y(SESS_ROW_H, UI_LINE_H_CJK16));
        lv_obj_set_width(s_sess_text[i], UI_W - UI_PAD * 2 - 34);
        // ★ 固定单行高度：长标题只能横向滚动，绝不允许换行把下一行挤走。
        lv_obj_set_height(s_sess_text[i], UI_LINE_H_CJK16);
        lv_label_set_long_mode(s_sess_text[i], LV_LABEL_LONG_DOT);

        s_sess_dot[i] = lv_obj_create(row);
        ui_theme_rect(s_sess_dot[i], UI_WARN);
        lv_obj_set_size(s_sess_dot[i], SESS_DOT_SIZE, SESS_DOT_SIZE);
        lv_obj_set_pos(s_sess_dot[i], UI_W - UI_PAD * 2 - SESS_DOT_SIZE - 6,
                       UI_CENTER_Y(SESS_ROW_H, SESS_DOT_SIZE));
        lv_obj_set_style_radius(s_sess_dot[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_add_flag(s_sess_dot[i], LV_OBJ_FLAG_HIDDEN);
    }

    // ══ 会话页（标题 + 状态 + 元信息 + 摘要）══
    s_session_panel = make_overlay("会话", &s_session_hint, &s_session_head, NULL);
    if (s_session_hint) lv_label_set_text(s_session_hint, "上长按返回 按住确定说话");
    if (s_session_head) {
        lv_obj_set_width(s_session_head, UI_W - UI_PAD * 2 - 8);
        // 标题只有一行：超长时横向循环滚动展示完整名称，不折行、不挤占下方内容。
        lv_obj_set_height(s_session_head, UI_LINE_H_CJK);
        lv_label_set_long_mode(s_session_head, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    }

    s_session_state = ui_theme_text(s_session_panel, "", UI_FONT_CJK16, UI_DIM);
    lv_obj_set_pos(s_session_state, UI_PAD, OV_TITLE_H + 10);
    lv_obj_set_width(s_session_state, UI_W - UI_PAD * 2);

    s_session_meta = ui_theme_text(s_session_panel, "", UI_FONT_CJK16, UI_DIM);
    lv_obj_set_pos(s_session_meta, UI_PAD, OV_TITLE_H + 34);
    lv_obj_set_width(s_session_meta, UI_W - UI_PAD * 2);
    lv_label_set_long_mode(s_session_meta, LV_LABEL_LONG_DOT);

    hrule(s_session_panel, OV_TITLE_H + 62);

    s_session_summary = ui_theme_text(s_session_panel, "", UI_FONT_CJK16, UI_FG);
    lv_obj_set_pos(s_session_summary, UI_PAD, OV_TITLE_H + 70);
    lv_obj_set_size(s_session_summary, UI_W - UI_PAD * 2, UI_H - OV_FOOT_H - (OV_TITLE_H + 74));
    lv_label_set_long_mode(s_session_summary, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(s_session_summary, UI_TEXT_LINE_SPACE, 0);
}

void app_screens_destroy(void)
{
    s_q_row_cache_valid = false;
    s_appr_sel_cached = -1;
    if (s_approval) lv_obj_delete(s_approval);
    if (s_balance) lv_obj_delete(s_balance);
    if (s_voice) lv_obj_delete(s_voice);
    if (s_question) lv_obj_delete(s_question);
    if (s_qvoice) lv_obj_delete(s_qvoice);
    if (s_settings) lv_obj_delete(s_settings);
    if (s_pair) lv_obj_delete(s_pair);
    if (s_sessions_panel) lv_obj_delete(s_sessions_panel);
    if (s_session_panel) lv_obj_delete(s_session_panel);
    s_approval = NULL;
    s_balance = NULL;
    s_voice = NULL;
    s_question = NULL;
    s_qvoice = NULL;
    s_settings = NULL; s_settings_hint = NULL;
    s_pair = NULL; s_pair_code_text = NULL; s_pair_sub_text = NULL;
    s_sessions_panel = NULL; s_sess_hint = NULL;
    for (int i = 0; i < AP_SESSION_MAX; i++) {
        s_sess_rows[i] = NULL; s_sess_bar[i] = NULL; s_sess_text[i] = NULL; s_sess_dot[i] = NULL;
    }
    s_session_panel = NULL; s_session_hint = NULL; s_session_head = NULL; s_session_state = NULL;
    s_session_meta = NULL; s_session_summary = NULL;
    s_sess_count = 0; s_sess_cache_valid = false;
    s_appr_session = NULL;
    s_set_pair_row = NULL; s_set_pair_bar = NULL; s_set_pair_text = NULL;
    for (int i = 0; i < AP_SETTINGS_ITEM_COUNT; i++) {
        s_set_rows[i] = NULL; s_set_bar[i] = NULL; s_set_text[i] = NULL;
    }
    s_balance_total = NULL; s_balance_lines = NULL;
    s_appr_tool = NULL; s_appr_hint = NULL;
    s_voice_text = NULL; s_voice_hint = NULL; s_voice_keys = NULL; s_voice_box = NULL;
    s_voice_foot_rule = NULL;
    s_q_text = NULL; s_q_hint = NULL;
    s_qvoice_text = NULL; s_qvoice_hint = NULL;
    for (int i = 0; i < APPR_OPTIONS; i++) {
        s_appr_option[i] = NULL; s_appr_opt_bar[i] = NULL; s_appr_opt_text[i] = NULL;
    }
    for (int i = 0; i < Q_ROW_MAX; i++) {
        s_q_rows[i] = NULL; s_q_row_bar[i] = NULL; s_q_row_text[i] = NULL;
    }
    s_current = AP_SCREEN_NONE;
}

// ── 显示控制 ──────────────────────────────────────────────────────────────

ap_screen_t app_screens_current(void) { return s_current; }

bool app_screens_approval_active(void) { return s_current == AP_SCREEN_APPROVAL; }

static lv_obj_t *panel_of(ap_screen_t which)
{
    switch (which) {
        case AP_SCREEN_BALANCE:    return s_balance;
        case AP_SCREEN_VOICE:      return s_voice;
        case AP_SCREEN_QUESTION:   return s_question;
        case AP_SCREEN_Q_VOICE:    return s_qvoice;
        case AP_SCREEN_PLAN:       return s_question;
        case AP_SCREEN_APPROVAL:   return s_approval;
        case AP_SCREEN_SESSIONS:   return s_sessions_panel;
        case AP_SCREEN_SESSION:    return s_session_panel;
        case AP_SCREEN_SETTINGS:   return s_settings;
        case AP_SCREEN_PAIR:       return s_pair;
        default:                   return NULL;
    }
}

static lv_obj_t *panel_of(ap_screen_t which);   // 定义在后面，这里先声明

// ── 内存诊断（排查"卡顿是不是内存不够"）───────────────────────────────────
// 切覆盖页时打一条：堆余量不足会让 LVGL 文本分配/布局变慢甚至静默失败。
// 切页频率低（用户操作驱动），不会刷爆日志；LVGL 池单独看 ——
// label 文本分配全走它，池内碎片率高时每次 set_text 都变贵。
static void log_mem(const char *where)
{
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "[mem] %s: heap_free=%u min_ever=%u largest=%u "
             "lvgl_free=%u lvgl_used=%u%% frag=%u%%",
             where,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)mon.free_size, (unsigned)mon.used_pct, (unsigned)mon.frag_pct);
}

static void hide_all_panels(void)
{
    // ★ 遍历**所有屏**（经 panel_of），不要硬编码列表：原先硬编码漏了新加的
    //   系统设置页，于是审批/追问弹出来时设置页还盖在上面（真机反馈：
    //   "在系统设置界面遇到状态变化，应优先显示其他"）。
    //   panel_of 已把 PLAN 映射到 s_question，重复隐藏同一对象无害。
    //   约定：新增覆盖页时把枚举值加在 AP_SCREEN_SETTINGS 之前，这里自动覆盖。
    for (int s = AP_SCREEN_NONE + 1; s <= AP_SCREEN_SETTINGS; s++) {
        lv_obj_t *panel = panel_of((ap_screen_t)s);
        if (panel) lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_panel(ap_screen_t which)
{
    lv_obj_t *target = panel_of(which);
    if (!target) return;
    // ★ 单覆盖层不变式：同一时刻只显示一个覆盖页。
    //   否则审批弹出会盖住识别结果卡，审批收起后旧卡还挂着而 s_current 已回
    //   NONE —— 按键会穿到主页面触发 PTT，屏幕却还显示着结果卡（文不对键）。
    hide_all_panels();
    lv_obj_remove_flag(target, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(target);
    s_current = which;
    log_mem("切覆盖页");
}

void app_screens_hide(ap_screen_t which)
{
    lv_obj_t *target = panel_of(which);
    if (target) lv_obj_add_flag(target, LV_OBJ_FLAG_HIDDEN);
    if (s_current == which) s_current = AP_SCREEN_NONE;

    // 协议 v2：从任何覆盖层退回时，若已有会话列表，主页就是会话列表。
    // （断开/清空时 s_sess_count=0，自然回落到旧的四态主页。）
    if (which != AP_SCREEN_NONE && which != AP_SCREEN_SESSIONS &&
        s_current == AP_SCREEN_NONE && s_sess_count > 0 && s_sessions_panel) {
        show_panel(AP_SCREEN_SESSIONS);
    }
}

// ── 余额页 ────────────────────────────────────────────────────────────────

void app_screens_show_balance(const char *total, const char *currency,
                              const char *recharge, const char *bonus, const char *today_used)
{
    if (!s_balance) return;

    char line[40];
    snprintf(line, sizeof(line), "%s %s", (currency && currency[0]) ? currency : "CNY",
             (total && total[0]) ? total : "--");
    lv_label_set_text(s_balance_total, line);

    char body[180];
    size_t used = 0;
    body[0] = '\0';
    if (recharge && recharge[0]) {
        used += snprintf(body + used, sizeof(body) - used, "充值  %s\n", recharge);
    }
    if (bonus && bonus[0]) {
        used += snprintf(body + used, sizeof(body) - used, "赠送  %s\n", bonus);
    }
    if (today_used && today_used[0]) {
        snprintf(body + used, sizeof(body) - used, "今日已用  %s", today_used);
    }
    lv_label_set_text(s_balance_lines, body[0] ? body : "暂无明细");

    // 只有用户请求过（长按下键）才弹出；主动推送的数据只更新上面的文本。
    if (s_balance_show_pending) {
        s_balance_show_pending = false;
        show_panel(AP_SCREEN_BALANCE);
    }
}

/** 用户长按下键请求余额：下一条余额数据到达时才弹出余额页。 */
void app_screens_request_balance(void)
{
    s_balance_show_pending = true;
}

// ── 识别结果卡 ────────────────────────────────────────────────────────────

/** 把语音动作上报给主机（{resultId, mode}）。成功则收起卡片；失败留在卡上重试。 */
static bool send_voice_action(const char *mode)
{
    char target_field[32] = "";
    if (s_voice_target[0]) {
        snprintf(target_field, sizeof(target_field), "\"target\":\"%s\",", s_voice_target);
    }
    snprintf(s_tx_buf, sizeof(s_tx_buf),
             "{\"type\":\"%s\",\"msgId\":0,%s\"resultId\":\"%s\",\"mode\":\"%s\"}",
             AP_MSG_VOICE_ACTION, target_field, s_voice_id, mode);
    const esp_err_t rc = ap_link_send_json(s_tx_buf, true);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "语音动作发送失败 rc=%d", (int)rc);
        // 结果卡底栏是隐藏的，失败提示走 toast（屏上一定看得见）
        app_ui_post_toast("发送失败，请重试", true);
        return false;
    }
    s_voice_sent = true;
    app_screens_hide(AP_SCREEN_VOICE);
    ESP_LOGI(TAG, "已上报语音动作：%s（%s）", mode, s_voice_id);
    return true;
}

void app_screens_show_voice_result(const char *result_id, const char *text)
{
    if (!s_voice) return;

    snprintf(s_voice_id, sizeof(s_voice_id), "%s", result_id ? result_id : "");
    char buf[VOICE_TEXT_MAX];
    clean(text, buf, sizeof(buf));
    lv_label_set_text(s_voice_text, buf[0] ? buf : "（没有识别到内容）");
    if (s_voice_box) lv_obj_scroll_to_y(s_voice_box, 0, LV_ANIM_OFF);
    s_voice_sent = false;
    s_voice_deadline_ms = now_ms() + VOICE_CARD_TTL_MS;

    // 底栏在结果卡上始终隐藏（见创建处说明）；这里只要保证三行说明可见
    if (s_voice_keys) lv_obj_remove_flag(s_voice_keys, LV_OBJ_FLAG_HIDDEN);
    show_panel(AP_SCREEN_VOICE);
    ESP_LOGI(TAG, "识别结果卡：%s（%u 字节）", s_voice_id, (unsigned)strlen(buf));
}

void app_screens_tick_voice(uint32_t now)
{
    if (s_current != AP_SCREEN_VOICE || s_voice_sent) return;
    if ((int32_t)(now - s_voice_deadline_ms) >= 0) {
        app_screens_hide(AP_SCREEN_VOICE);
        ESP_LOGI(TAG, "识别结果卡 %u 秒无操作，自动收起（文本仍在主机语音列表）",
                 (unsigned)(VOICE_CARD_TTL_MS / 1000));
    }
}

// ── 追问 / 计划评审（docs/06 §3/§4）────────────────────────────────────────
//
// 主机持有整批真相，设备一次只显示一题：勾选变化即时回发 question.pick，
// 主机累计；提交/取消/要求修改走 question.answer 的三个动作值
// （submit=交答案 / cancel=交回电脑端 / reject=要求修改，拒绝等待）。

static void refresh_question_rows(void);
static void refresh_question_hint(void);
static void consume_q_voice_result(void);   // LVGL 任务里消费语音识别结果
static bool send_q_pick(bool skipped);

/** 行模型（用户决策 2026-10-06）：选项 + 其他 + [下一题] + 跳过。
 *  没有"提交"行 —— 单选单击即执行（末题直接提交），多选题长按确定提交。
 *  行号与行文本在 app_question_logic（纯逻辑 + host 测试），这里只取当前题面。 */
static int q_row_count(void)
{
    return ap_q_row_count(s_q.is_plan, s_q_option_count, s_q.index, s_q.total);
}

static int q_custom_row(void)
{
    return ap_q_row_custom(s_q.is_plan, s_q_option_count);
}

static bool q_has_next_row(void)
{
    return ap_q_row_has_next(s_q.is_plan, s_q.index, s_q.total);
}

static int q_next_row(void)
{
    return ap_q_row_next(s_q.is_plan, s_q_option_count, s_q.index, s_q.total);
}

static int q_skip_row(void)
{
    return ap_q_row_skip(s_q.is_plan, s_q_option_count, s_q.index, s_q.total);
}

static void q_finish(const char *toast_text, bool urgent)
{
    app_screens_hide(AP_SCREEN_QUESTION);
    app_screens_hide(AP_SCREEN_Q_VOICE);
    app_screens_hide(AP_SCREEN_PLAN);
    if (toast_text) app_ui_post_toast(toast_text, urgent);
}

/** 出站发送 + 失败反馈（失败不静默，底栏给"重试"指引）。 */
static bool send_q_raw(const char *json, const char *what, lv_obj_t *hint)
{
    const esp_err_t rc = ap_link_send_json(json, true);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "%s 发送失败 rc=%d", what, (int)rc);
        if (hint) {
            lv_label_set_text(hint, "发送失败，请重试");
            lv_obj_set_style_text_color(hint, lv_color_hex(UI_ERR), 0);
        }
        return false;
    }
    return true;
}

static bool send_q_pick(bool skipped)
{
    char buf[512];
    ap_q_build_pick_json(buf, sizeof(buf), s_q.call_id, s_q.qid,
                         s_q_labels, s_q_selected, s_q_option_count,
                         s_q_custom, skipped);
    return send_q_raw(buf, "question.pick", s_q_hint);
}

static bool send_q_answer(const char *action)
{
    char buf[160];
    ap_q_build_answer_json(buf, sizeof(buf), s_q.call_id, action);
    return send_q_raw(buf, "question.answer", s_q_hint);
}

static void send_q_nav(int index)
{
    char buf[128];
    ap_q_build_nav_json(buf, sizeof(buf), s_q.call_id, index);
    (void)send_q_raw(buf, "question.nav", s_q_hint);
}

/** 计划批准：以 intent.approve 的 label 作答（不是普通选项，docs/06 §1.3）。 */
static void plan_approve(void)
{
    if (s_q_option_count == 0) {
        // 计划评审题的选项由主机给；极端情况下没有选项时合成一个批准位
        snprintf(s_q_labels[0], AP_Q_LABEL_SIZE, "%s",
                 s_q.approve_label[0] ? s_q.approve_label : "同意执行");
        s_q_option_count = 1;
    }
    int idx = -1;
    for (int i = 0; i < s_q_option_count; i++) {
        if (strcmp(s_q_labels[i], s_q.approve_label) == 0) { idx = i; break; }
    }
    if (idx < 0) {
        // approve label 不在截断后的选项表里：把它作为唯一选中项发出去，
        // 主机端按原始 label 校验，不会错配。
        snprintf(s_q_labels[0], AP_Q_LABEL_SIZE, "%s", s_q.approve_label);
        s_q_option_count = 1;
        idx = 0;
    }
    ap_q_select_single(s_q_selected, s_q_option_count, idx);
    if (send_q_pick(false) && send_q_answer("submit")) {
        alert_play(ALERT_DECIDED, alert_get_setting());
        q_finish("已同意执行", false);
    }
}

static void refresh_question_rows(void)
{
    const int rows = q_row_count();
    for (int i = 0; i < Q_ROW_MAX; i++) {
        const bool visible = (i < rows);
        const bool sel = (i == s_q_cursor);
        const uint16_t state = (uint16_t)((visible ? 1u : 0u) | (sel ? 2u : 0u));

        // 行文本由纯逻辑产出（选项行含选中标记，标记变化自然落在文本对比里）
        char buf[AP_Q_ROW_TEXT_MAX];
        buf[0] = '\0';
        if (visible) {
            (void)ap_q_row_text(buf, sizeof(buf), s_q.is_plan, s_q.multi_select,
                                s_q_selected, s_q_labels, s_q_option_count,
                                s_q.index, s_q.total, s_q_custom, i);
        }

        // ★ 增量刷新：与上次真正写进 LVGL 的状态一致就整行跳过。
        //   光标移动只改 2 行、勾选只改 1 行，而不是整列表 8 行。
        if (s_q_row_cache_valid &&
            s_q_row_state[i] == state &&
            strcmp(s_q_row_cache[i], buf) == 0) {
            continue;
        }
        snprintf(s_q_row_cache[i], sizeof(s_q_row_cache[i]), "%s", buf);
        s_q_row_state[i] = state;

        if (!visible) {
            lv_obj_add_flag(s_q_rows[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_q_rows[i], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_q_row_text[i], buf);
        // 选中行高亮（沿用审批页调过的对比度：色条 4→8px + 行底色加亮）
        lv_obj_set_size(s_q_row_bar[i], sel ? 8 : 4, OV_Q_ROW_H);
        lv_obj_set_style_bg_color(s_q_row_bar[i], lv_color_hex(UI_ACCENT), 0);
        lv_obj_set_style_bg_opa(s_q_row_bar[i], sel ? LV_OPA_COVER : LV_OPA_50, 0);
        lv_obj_set_style_bg_color(s_q_rows[i], lv_color_hex(sel ? UI_ROW_SEL : UI_BG), 0);
        lv_obj_set_style_text_color(s_q_row_text[i], lv_color_hex(sel ? UI_FG : UI_DIM), 0);
    }
    s_q_row_cache_valid = true;
    // 不加动画：滚动动画会让整个列表逐帧重绘（卡顿放大器）；行数在可视区内时
    // scroll_to_view 本身什么都不做。
    lv_obj_scroll_to_view(s_q_rows[s_q_cursor], LV_ANIM_OFF);
}

static void refresh_question_hint(void)
{
    if (!s_q_hint) return;
    const char *text;
    uint32_t color = UI_DIM;
    if (s_q.is_plan) {
        text = "上/下选择 确定执行";
    } else if (s_q.multi_select) {
        text = "单击勾选 长按提交";   // 多选题没有提交行，长按确定提交
    } else {
        text = "上/下选择 确定执行";
    }
    // 提示行始终重设：发送失败路径会把这里换成"发送失败，请重试"，
    // 缓存文本指针会让失败提示永远留在屏上。单行小标签的代价可忽略。
    lv_label_set_text(s_q_hint, text);
    lv_obj_set_style_text_color(s_q_hint, lv_color_hex(color), 0);
}
/** 显示一题（question.req 到达；也用于 question.nav 换题）。 */
static void show_question(const ap_question_msg_t *q)
{
    if (!s_question) return;

    s_q = *q;
    s_q_option_count = ap_q_split_options(s_q.options_csv, s_q_labels, AP_Q_MAX_OPTIONS);
    memset(s_q_selected, 0, sizeof(s_q_selected));
    // "Recommended" 首选项预选为草稿（对齐 DSH 语义：预选 ≠ 已提交）
    if (!s_q.multi_select && s_q.recommended_index >= 0 && s_q.recommended_index < s_q_option_count) {
        ap_q_select_single(s_q_selected, s_q_option_count, s_q.recommended_index);
    }
    s_q_custom[0] = '\0';
    s_q_voice_mode = false;
    // 计划待审默认停在「同意执行」（用户要求）；选择题仍从第一项开始。
    s_q_cursor = s_q.is_plan ? 1 : 0;

    char title[48];
    if (s_q.is_plan) {
        snprintf(title, sizeof(title), "计划待审");
    } else {
        snprintf(title, sizeof(title), "问题 %d/%d", s_q.index + 1, s_q.total > 0 ? s_q.total : 1);
    }
    if (s_q_title) lv_label_set_text(s_q_title, title);

    // 只有一行极短摘要（长文本问题因此消失：正文都在电脑卡片上）
    char body[AP_QUESTION_SUMMARY_MAX];
    clean(s_q.summary, body, sizeof(body));
    lv_label_set_text(s_q_text, body[0] ? body : "电脑上有待回答的问题");

    refresh_question_rows();
    refresh_question_hint();
    show_panel(s_q.is_plan ? AP_SCREEN_PLAN : AP_SCREEN_QUESTION);
    ESP_LOGI(TAG, "追问显示：%s（%d/%d，%s）", s_q.qid, s_q.index + 1,
             s_q.total > 0 ? s_q.total : 1, s_q.is_plan ? "plan" : "choice");
}

void app_screens_tick_question(uint32_t now)
{
    // 倒计时条目前只做视觉提示（可在后续加 lv_bar）；真正的超时由主机裁决
    // 并回 question.done —— 设备不自作主张替用户交答案或取消。
    (void)now;
    // 跨任务投递的语音识别结果在这里落地（本函数运行在 LVGL 任务里）
    consume_q_voice_result();
}

// ── 追问/计划评审的按键 ────────────────────────────────────────────────────

/** 本题结束后：还有下一题就换题，否则提交整批。 */
static void q_advance_or_submit(void)
{
    if (s_q.index + 1 < s_q.total) {
        send_q_nav(s_q.index + 1);
    } else if (send_q_answer("submit")) {
        q_finish("已提交", false);
    }
}

/** 进入语音输入页（自定义答案）：按住确定说话、松手识别，上键单击清除、长按放弃回主页。 */
static void show_qvoice(void)
{
    if (!s_qvoice) return;
    s_q_voice_mode = true;
    if (s_qvoice_text) {
        lv_label_set_text(s_qvoice_text, "按住确定说话");
        lv_obj_set_style_text_color(s_qvoice_text, lv_color_hex(UI_FG), 0);
    }
    if (s_qvoice_hint) {
        lv_label_set_text(s_qvoice_hint, "上键清除 长按放弃");
        lv_obj_set_style_text_color(s_qvoice_hint, lv_color_hex(UI_DIM), 0);
    }
    show_panel(AP_SCREEN_Q_VOICE);
}

/** 语音输入页按键：PTT 录音 + 放弃返回选择界面。 */
static bool handle_qvoice_key(int btn, int event)
{
    if (btn == APP_UI_BTN_UP && event == APP_UI_EV_LONG) {
        // 长按上键 = **放弃本次语音输入并回主页面**（用户要求）：
        // 停止录音、丢弃识别结果与已输入内容，并把"已清空"同步给主机；
        // 该追问在主机侧仍然挂起，可以继续在电脑上回答。
        app_voice_stop();   // 不前置判断 recording()：任务可能还没醒
        s_q_voice_mode = false;
        s_q_custom[0] = '\0';
        (void)send_q_pick(false);
        app_screens_hide(AP_SCREEN_Q_VOICE);
        app_screens_hide(s_q.is_plan ? AP_SCREEN_PLAN : AP_SCREEN_QUESTION);
        app_ui_post_toast("已放弃语音输入", false);
        return true;
    }
    if (btn == APP_UI_BTN_UP && event == APP_UI_EV_CLICK) {
        // 单击上键 = **清除当前输入内容**（用户要求）：清空自定义答案并同步主机，留在本页
        s_q_custom[0] = '\0';
        (void)send_q_pick(false);
        refresh_question_rows();
        app_ui_post_toast("已清除输入内容", false);
        return true;
    }
    if (btn == APP_UI_BTN_OK) {
        if (event == APP_UI_EV_PRESS) {
            (void)app_voice_start();
            if (s_qvoice_text) {
                lv_label_set_text(s_qvoice_text, "正在录音…");
                lv_obj_set_style_text_color(s_qvoice_text, lv_color_hex(UI_WARN), 0);
            }
            if (s_qvoice_hint) lv_label_set_text(s_qvoice_hint, "松手结束");
        } else if (event == APP_UI_EV_RELEASE) {
            // ★ 不前置判断 recording()：快速点按时语音任务可能还没醒（state 仍 IDLE），
            //   前置判断会漏掉这次停止 —— 录音一路录到 VAD/上限（主页 PTT 同款约定）。
            app_voice_stop();
            if (s_qvoice_text) {
                lv_label_set_text(s_qvoice_text, "识别中…");
                lv_obj_set_style_text_color(s_qvoice_text, lv_color_hex(UI_DIM), 0);
            }
            if (s_qvoice_hint) lv_label_set_text(s_qvoice_hint, "上键清除 长按放弃");
        }
        // CLICK 是松手后的余波：吃掉，别漏到主页触发其它动作
        return true;
    }
    return true;   // 语音页期间吃掉所有按键，避免误触主页
}

static bool handle_question_key(int btn, int event)
{
    const int rows = q_row_count();

    if (btn == APP_UI_BTN_UP && event == APP_UI_EV_LONG) {
        // 取消整个提问 → 交给电脑卡片（cancel ≠ reject：后者是"要求修改"）
        if (send_q_answer("cancel")) q_finish("已取消，请到电脑上继续", false);
        return true;
    }
    // ★ 长按确定 = 提交整批（多选题的出口）。单选末题单击即提交，用不到；
    //   设备上不再有"提交"行 —— 选项行之外只保留 其他 / [下一题] / 跳过。
    if (btn == APP_UI_BTN_OK && event == APP_UI_EV_LONG) {
        if (s_q.is_plan) return true;   // 计划页长按无动作（防误触）
        if (send_q_pick(false) && send_q_answer("submit")) q_finish("已提交", false);
        return true;
    }
    if (event != APP_UI_EV_CLICK) return true;


    if (btn == APP_UI_BTN_UP) {
        s_q_cursor = ap_q_cursor_move(s_q_cursor, rows, -1);
        refresh_question_rows();
        return true;
    }
    if (btn == APP_UI_BTN_DOWN) {
        s_q_cursor = ap_q_cursor_move(s_q_cursor, rows, 1);
        refresh_question_rows();
        return true;
    }
    if (btn != APP_UI_BTN_OK) return true;

    // —— OK 单击 ——
    if (s_q.is_plan) {
        if (s_q_cursor == 0) {
            // 要求修改 = 拒绝等待（ASK_CANCELLED），不是选项作答（docs/06 §1.3）
            if (send_q_answer("reject")) {
                alert_play(ALERT_DECIDED, alert_get_setting());
                q_finish("请在电脑输入修改意见", false);
            }
        } else {
            // 批准计划：与审批页一致，**单击确认**（用户决策 2026-10-06）
            plan_approve();
        }
        return true;
    }

    if (s_q_cursor < s_q_option_count) {
        if (s_q.multi_select) {
            ap_q_select_toggle(s_q_selected, s_q_option_count, s_q_cursor);
            (void)send_q_pick(false);
            refresh_question_rows();
        } else {
            // 单选：选中即前进（对齐 DSH"单击即前进"），最后一题直接提交
            ap_q_select_single(s_q_selected, s_q_option_count, s_q_cursor);
            if (send_q_pick(false)) {
                if (s_q.index + 1 < s_q.total) {
                    send_q_nav(s_q.index + 1);
                } else if (send_q_answer("submit")) {
                    q_finish("已提交", false);
                }
            }
            refresh_question_rows();
        }
        return true;
    }
    if (s_q_cursor == q_custom_row()) {
        // 自定义行：按确定 = 进**独立的语音输入页**（可放弃返回，用户决策）
        show_qvoice();
        return true;
    }
    if (q_has_next_row() && s_q_cursor == q_next_row()) {
        // 下一题：已上报的选择留在主机侧，这里只换题
        if (send_q_pick(false)) q_advance_or_submit();
        return true;
    }

    // 剩下的只可能是最后一行「跳过此题」（行模型：选项 / 其他 / [下一题] / 跳过）
    if (s_q_cursor != q_skip_row()) return true;
    // 跳过此题：清掉本题草稿（对齐官方卡片的 Skip 语义）
    memset(s_q_selected, 0, sizeof(s_q_selected));
    s_q_custom[0] = '\0';
    if (send_q_pick(true)) q_advance_or_submit();
    return true;
}


bool app_question_on_voice_result(const char *text)
{
    if (!s_q_voice_mode) return false;
    s_q_voice_mode = false;

    // ★ 本函数跑在 **app_task**（链路消息队列的消费者），不是 LVGL 任务 ——
    //   这里只允许写结构 + 置脏标志。真正的界面动作由 LVGL 任务里的
    //   app_screens_tick_question() 消费（与 post_* 系列同一套线程模型）。
    utf8_copy_clamped(text, s_q_voice_result_text, sizeof(s_q_voice_result_text));
    s_q_voice_result_dirty = true;
    return true;
}

/** 消费语音识别结果（**只在 LVGL 任务里调用**）。 */
static void consume_q_voice_result(void)
{
    if (!s_q_voice_result_dirty) return;
    s_q_voice_result_dirty = false;

    if (s_current != AP_SCREEN_Q_VOICE) {
        // 语音页已被收起（超时/电脑先答/用户放弃）：结果无处安放，明确丢弃
        app_ui_post_toast("语音输入已丢弃（问题已结束）", true);
        return;
    }
    clean(s_q_voice_result_text, s_q_custom, sizeof(s_q_custom));
    (void)send_q_pick(false);
    // ★ 识别完成 → 自动回到选择界面：自定义行显示识别文本，
    //   用户可以接着选别的选项，或者再按确定重新录一次。
    show_panel(s_q.is_plan ? AP_SCREEN_PLAN : AP_SCREEN_QUESTION);
    refresh_question_rows();
    refresh_question_hint();
    app_ui_post_toast("已录入，可继续选择", false);
}

// ── 审批页 ────────────────────────────────────────────────────────────────

/** 按当前选择与确认状态刷新两个选项的外观与提示文案。 */
static void refresh_approval_options(void)
{
    // ★ 增量刷新：行外观只随选中态变化，旧渲染态没变的行不碰 LVGL
    //   （与追问页同一理由：无条件全量重写是小屏上可感知的卡顿来源）。
    for (int i = 0; i < APPR_OPTIONS; i++) {
        const bool sel = (i == s_appr_selected);
        if (s_appr_sel_cached >= 0 && sel == (i == s_appr_sel_cached)) continue;
        // 选中的色条明显加宽 —— 小屏上"选的是哪一个"必须一眼可见
        lv_obj_set_size(s_appr_opt_bar[i], sel ? 8 : 4, OV_APPR_OPT_H);
        lv_obj_set_style_bg_color(s_appr_opt_bar[i], lv_color_hex(s_options[i].color), 0);
        lv_obj_set_style_bg_opa(s_appr_opt_bar[i], sel ? LV_OPA_COVER : LV_OPA_50, 0);
        lv_obj_set_style_bg_color(s_appr_option[i], lv_color_hex(sel ? UI_ROW_SEL : UI_BG), 0);
        lv_obj_set_style_text_color(s_appr_opt_text[i], lv_color_hex(sel ? UI_FG : UI_DIM), 0);
    }
    s_appr_sel_cached = s_appr_selected;

    if (!s_appr_hint) return;
    // 单击即执行（用户决策 2026-10-06）：提示只说此刻能做的事。
    // 始终重设：发送失败路径会把它换成"发送失败，请重试"。
    lv_label_set_text(s_appr_hint, "上/下选择  确定执行");
    lv_obj_set_style_text_color(s_appr_hint, lv_color_hex(UI_DIM), 0);
}

void app_screens_show_approval(const char *request_id, const char *summary,
                               const char *session_title, bool high_risk, uint32_t timeout_ms)
{
    if (!s_approval) return;

    snprintf(s_appr_id, sizeof(s_appr_id), "%s", request_id ? request_id : "");
    // 不再倒计时（用户要求）：审批页一直停留，等到用户在电脑端或设备端作答。
    (void)timeout_ms;
    // ★ 默认停在「运行一次」（选项表第 0 项）。
    s_appr_selected = 0;
    s_appr_submitted = false;

    // 只有一行摘要：正文（工具详情/原因）都在电脑卡片上，设备只做快捷选择
    char buf[80];
    clean(summary, buf, sizeof(buf));
    lv_label_set_text(s_appr_tool, buf[0] ? buf : "电脑上有待审批请求");

    // 没有倒计时条之后，风险等级靠摘要行的颜色表达（高风险=红，普通=黄）
    if (s_appr_tool) {
        lv_obj_set_style_text_color(s_appr_tool, lv_color_hex(high_risk ? UI_ERR : UI_WARN), 0);
    }

    // 协议 v2：告诉用户"是哪个会话在等"（旧主机不带该字段 → 整行隐藏）。
    if (s_appr_session) {
        if (session_title && session_title[0]) {
            char line[AP_SESSION_TITLE_MAX + 16];
            snprintf(line, sizeof(line), "会话：%s", session_title);
            lv_label_set_text(s_appr_session, line);
            lv_obj_remove_flag(s_appr_session, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_appr_session, LV_OBJ_FLAG_HIDDEN);
        }
    }

    refresh_approval_options();
    show_panel(AP_SCREEN_APPROVAL);

    ESP_LOGI(TAG, "审批请求：%s（%s）%s", request_id ? request_id : "?",
             buf[0] ? buf : "?", high_risk ? " 高风险" : "");
}

/** 把审批决定发给主机。在 LVGL 任务里调用，立即发送。 */
static void send_decision(const approval_option_t *opt)
{
    snprintf(s_tx_buf, sizeof(s_tx_buf),
             "{\"type\":\"%s\",\"msgId\":0,\"id\":\"%s\",\"decision\":\"%s\",\"scope\":\"%s\"}",
             AP_MSG_APPROVE, s_appr_id, opt->decision, opt->scope);
    const esp_err_t rc = ap_link_send_json(s_tx_buf, true);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "审批决定发送失败 rc=%d", (int)rc);
        if (s_appr_hint) {
            lv_label_set_text(s_appr_hint, "发送失败，请重试");
            lv_obj_set_style_text_color(s_appr_hint, lv_color_hex(UI_ERR), 0);
        }
    } else {
        s_appr_submitted = true;
        // 审批"已选择"用**专属提示音**：与"待审批"的急促双响、"任务完成"的舒缓单响区分开。
        alert_play(ALERT_DECIDED, alert_get_setting());
        // 提交后立刻给一句反馈：主机回 approve.result 会有往返延迟，
        // 没有这行字用户会以为按键没生效而重复按。
        if (s_appr_hint) {
            lv_label_set_text(s_appr_hint, "已提交，等待结果");
            lv_obj_set_style_text_color(s_appr_hint, lv_color_hex(UI_ACCENT), 0);
        }
        ESP_LOGI(TAG, "已提交审批：%s（scope=%s）", opt->label, opt->scope);
    }
}

/**
 * 倒计时驱动。返回是否仍在审批中。
 *
 * ★ 超时按「拒绝」上报，**绝不自动放行**。代价不对称：超时拒绝的后果是用户
 *   重发一次请求；超时允许的后果可能是文件被写坏。
 */
// ── 按键 ──────────────────────────────────────────────────────────────────

// ══ 系统设置页 ══════════════════════════════════════════════════════════════
// 交互：上/下选择、**单击确定切换当前项档位**、长按确定或长按上键退出回主页。
// 改档立即生效并写 NVS（app_settings 负责持久化）；改音量档时立刻试听一声 ——
// 调音量听不到反馈等于闭着眼调。
static int settings_value_of(ap_settings_item_t item)
{
    const ap_settings_t *set = ap_settings_get();
    switch (item) {
    case AP_SETTINGS_SCREEN_OFF: return set->screen_off_min;
    case AP_SETTINGS_BRIGHTNESS: return set->brightness;
    case AP_SETTINGS_AUTO_OFF:   return set->auto_off_min;
    default:                     return set->alert_volume;
    }
}

static void refresh_settings_rows(void)
{
    for (int i = 0; i < AP_SETTINGS_ITEM_COUNT; i++) {
        const ap_settings_item_def_t *def = ap_settings_item((ap_settings_item_t)i);
        const int idx = ap_settings_index_of(def, settings_value_of((ap_settings_item_t)i));
        char buf[64];
        snprintf(buf, sizeof(buf), "%s：%s", def->name, def->labels[idx]);
        if (s_set_text[i]) lv_label_set_text(s_set_text[i], buf);

        const bool sel = (i == s_set_cursor);
        if (s_set_bar[i]) {
            lv_obj_set_style_bg_color(s_set_bar[i], lv_color_hex(sel ? UI_ACCENT : UI_LINE), 0);
            lv_obj_set_style_bg_opa(s_set_bar[i], sel ? LV_OPA_COVER : LV_OPA_50, 0);
        }
        if (s_set_rows[i]) {
            lv_obj_set_style_bg_color(s_set_rows[i], lv_color_hex(sel ? UI_ROW_SEL : UI_BG), 0);
        }
    }

    // 配对行（最后一行，不是档位项）。两行文本：
    //   状态行「配对：已配对/未配对」+ 主机名行（已配对且有主机名时）。
    //   以前是单行"配对：已配对（主机名）"，主机名一长就被省略号吃掉。
    if (s_set_pair_text) {
        char buf[64];   // host 缓冲 32B + "配对：已配对\n"（18B）放得下
        if (ap_pair_is_paired()) {
            const char *host = ap_pair_host_name();
            if (host[0]) {
                snprintf(buf, sizeof(buf), "配对：已配对\n%s", host);
            } else {
                snprintf(buf, sizeof(buf), "配对：已配对");
            }
        } else {
            snprintf(buf, sizeof(buf), "配对：未配对");
        }
        lv_label_set_text(s_set_pair_text, buf);
    }
    const bool pair_sel = (s_set_cursor == AP_SETTINGS_ITEM_COUNT);
    if (s_set_pair_bar) {
        lv_obj_set_style_bg_color(s_set_pair_bar,
                                  lv_color_hex(pair_sel ? UI_ACCENT : UI_LINE), 0);
        lv_obj_set_style_bg_opa(s_set_pair_bar, pair_sel ? LV_OPA_COVER : LV_OPA_50, 0);
    }
    if (s_set_pair_row) {
        lv_obj_set_style_bg_color(s_set_pair_row,
                                  lv_color_hex(pair_sel ? UI_ROW_SEL : UI_BG), 0);
    }
}

/** 把配对码页刷到最新（进页或收到新码时调用）。 */
static void refresh_pair_page(void)
{
    if (!s_pair_code_text) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "配对码\n%s", ap_pair_code_str());
    lv_label_set_text(s_pair_code_text, buf);
    if (s_pair_sub_text) {
        char sub[64];
        snprintf(sub, sizeof(sub), "设备 %s · 长按确定关闭", ap_pair_device_id());
        lv_label_set_text(s_pair_sub_text, sub);
    }
}

/** 跨任务投递：BLE 任务里不能碰 LVGL（约定见 app_screens.h）。 */
void app_screens_post_pair_code(const char *code, const char *device_id)
{
    snprintf(s_post_pair_code, sizeof(s_post_pair_code), "%s", code ? code : "");
    snprintf(s_post_pair_id, sizeof(s_post_pair_id), "%s", device_id ? device_id : "");
    s_pair_dirty = true;
}

void app_screens_show_settings(void)
{
    s_set_cursor = 0;
    refresh_settings_rows();
    show_panel(AP_SCREEN_SETTINGS);
    ESP_LOGI(TAG, "系统设置页打开");
}

static bool handle_settings_key(int btn, int event)
{
    if (event == APP_UI_EV_LONG && (btn == APP_UI_BTN_OK || btn == APP_UI_BTN_UP)) {
        app_screens_hide(AP_SCREEN_SETTINGS);
        ESP_LOGI(TAG, "系统设置页关闭（返回主页）");
        return true;
    }
    if (event != APP_UI_EV_CLICK) return true;

    const int rows = AP_SETTINGS_ITEM_COUNT + 1;   // 末行是「配对」
    if (btn == APP_UI_BTN_UP) {
        s_set_cursor = ap_settings_cycle(s_set_cursor, rows, -1);
        refresh_settings_rows();
        return true;
    }
    if (btn == APP_UI_BTN_DOWN) {
        s_set_cursor = ap_settings_cycle(s_set_cursor, rows, 1);
        refresh_settings_rows();
        return true;
    }
    if (btn == APP_UI_BTN_OK && s_set_cursor == AP_SETTINGS_ITEM_COUNT) {
        // 配对行：未配对 → 弹出配对码页；已配对 → 提示（解除配对由主机侧发起）
        if (ap_pair_is_paired()) {
            app_ui_post_toast("已配对，如需重配请在电脑上解除", false);
        } else {
            refresh_pair_page();
            show_panel(AP_SCREEN_PAIR);
            ESP_LOGI(TAG, "显示配对码页（%s）", ap_pair_code_str());
        }
        return true;
    }
    if (btn == APP_UI_BTN_OK) {
        const ap_settings_item_t item = (ap_settings_item_t)s_set_cursor;
        const ap_settings_item_def_t *def = ap_settings_item(item);
        const int cur = settings_value_of(item);
        const int next = def->values[ap_settings_cycle(ap_settings_index_of(def, cur), def->count, 1)];
        (void)ap_settings_set_value(item, next);
        if (item == AP_SETTINGS_BRIGHTNESS) {
            // 亮度立刻生效：改完马上能看见，不用等下次亮屏
            app_ui_apply_brightness();
        }
        if (item == AP_SETTINGS_ALERT_VOLUME) {
            const uint8_t vol = (uint8_t)settings_value_of(AP_SETTINGS_ALERT_VOLUME);
            alert_set_volume(vol);
            if (vol > 0) alert_play(ALERT_DECIDED, true);   // 试听一声
        }
        refresh_settings_rows();
        ESP_LOGI(TAG, "%s → %s", def->name, def->labels[ap_settings_index_of(def, next)]);
        return true;
    }
    return true;
}


// ── 多会话：渲染与按键（协议 v2）──────────────────────────────────────────

static uint32_t session_color(uint8_t state)
{
    switch (state) {
    case AP_SESSION_STATE_RUNNING: return UI_ACCENT;   // 绿 = 运行中
    case AP_SESSION_STATE_WAITING: return UI_WARN;     // 黄 = 等你处理
    case AP_SESSION_STATE_ERROR:
    case AP_SESSION_STATE_ABORTED: return UI_ERR;      // 红 = 出错/中断
    default: return UI_DIM;                            // 灰 = 空闲/未知
    }
}

static const char *session_state_text(uint8_t state)
{
    switch (state) {
    case AP_SESSION_STATE_RUNNING: return "运行中";
    case AP_SESSION_STATE_WAITING: return "等待你处理";
    case AP_SESSION_STATE_ERROR: return "出错";
    case AP_SESSION_STATE_ABORTED: return "已中断";
    case AP_SESSION_STATE_IDLE: return "空闲";
    default: return "-";
    }
}

static int sess_find_view(const char *handle)
{
    if (!handle || !handle[0]) return -1;
    for (int i = 0; i < s_sess_count; i++) {
        if (strcmp(s_sess_view[i].handle, handle) == 0) return i;
    }
    return -1;
}

/** 增量刷新列表行：只差量写 LVGL（小屏上全量重绘 8 行是感知得到的卡顿）。 */
static void refresh_sess_rows(void)
{
    if (!s_sessions_panel) return;
    if (s_sess_count < 0) s_sess_count = 0;
    if (s_sess_count > AP_SESSION_MAX) s_sess_count = AP_SESSION_MAX;
    if (s_sess_count == 0) s_sess_cursor = 0;
    else if (s_sess_cursor < 0) s_sess_cursor = 0;
    else if (s_sess_cursor >= s_sess_count) s_sess_cursor = s_sess_count - 1;

    for (int i = 0; i < AP_SESSION_MAX; i++) {
        const bool visible = i < s_sess_count;
        const bool selected = visible && i == s_sess_cursor;
        const bool unread = visible && s_sess_view[i].unread;
        const uint8_t flags = (uint8_t)((visible ? 1 : 0) | (selected ? 2 : 0) | (unread ? 4 : 0));
        const char *title = visible ? (s_sess_view[i].title[0] ? s_sess_view[i].title : "会话") : "";

        if (s_sess_cache_valid && flags == s_sess_flags[i] &&
            strcmp(title, s_sess_hash[i]) == 0) {
            continue;
        }
        s_sess_flags[i] = flags;
        snprintf(s_sess_hash[i], sizeof(s_sess_hash[i]), "%s", title);

        if (!visible) {
            lv_obj_add_flag(s_sess_rows[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_sess_rows[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_sess_rows[i], lv_color_hex(selected ? UI_ROW_SEL : UI_BG), 0);
        lv_obj_set_size(s_sess_bar[i], selected ? 8 : 4, SESS_ROW_H);
        lv_obj_set_style_bg_color(s_sess_bar[i],
                                  lv_color_hex(session_color(s_sess_view[i].state)), 0);
        // 只有选中的行滚动（8 行同时跑动画在 C3 上会明显卡顿）；其余行省略号截断。
        const lv_label_long_mode_t mode = selected ? LV_LABEL_LONG_MODE_SCROLL_CIRCULAR
                                                   : LV_LABEL_LONG_DOT;
        if (lv_label_get_long_mode(s_sess_text[i]) != mode) {
            lv_label_set_long_mode(s_sess_text[i], mode);
        }
        lv_label_set_text(s_sess_text[i], title);
        lv_obj_set_style_text_color(s_sess_text[i], lv_color_hex(selected ? UI_FG : UI_FG_DIM), 0);
        if (unread) lv_obj_remove_flag(s_sess_dot[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_sess_dot[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_sess_cache_valid = true;
}

static void show_sessions_panel(void)
{
    if (!s_sessions_panel) return;
    if (s_current != AP_SCREEN_SESSIONS) show_panel(AP_SCREEN_SESSIONS);
    s_sess_cache_valid = false;
    refresh_sess_rows();
}


/** 往定长缓冲追加一段文本（可带分隔符）；超长按字节截断（这里只用于 ASCII 元信息）。 */
static void meta_append(char *dst, size_t cap, size_t *used, const char *sep, const char *text)
{
    if (!dst || cap == 0 || !used || !text || !text[0]) return;
    const size_t sep_len = (*used > 0 && sep) ? strlen(sep) : 0;
    size_t add = strlen(text);
    if (*used + sep_len >= cap) return;
    if (*used + sep_len + add >= cap) {
        add = cap - 1 - *used - sep_len;
    }
    if (sep_len) {
        memcpy(dst + *used, sep, sep_len);
        *used += sep_len;
    }
    memcpy(dst + *used, text, add);
    *used += add;
    dst[*used] = '\0';
}

/** 会话页内容刷新：优先用 detail（step/tool/摘要），否则只用列表行的标题与状态。 */
static void refresh_session_page(void)
{
    if (!s_session_panel) return;
    const int idx = sess_find_view(s_session_handle);
    uint8_t state = AP_SESSION_STATE_UNKNOWN;
    const char *title = "会话";
    if (idx >= 0) {
        state = s_sess_view[idx].state;
        if (s_sess_view[idx].title[0]) title = s_sess_view[idx].title;
    }

    const ap_session_detail_t *d = &s_session_detail_view;
    if (d->valid && strcmp(d->handle, s_session_handle) == 0) {
        if (d->title[0]) title = d->title;
        state = d->state;
        char meta[96] = "";
        size_t n = 0;
        if (d->step >= 0) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "step %d/%d", d->step, d->step_total);
            meta_append(meta, sizeof(meta), &n, "", tmp);
        }
        if (app_session_show_elapsed() && d->elapsed_ms > 0) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "%lus", (unsigned long)(d->elapsed_ms / 1000));
            meta_append(meta, sizeof(meta), &n, "  ", tmp);
        }
        if (d->tool[0]) meta_append(meta, sizeof(meta), &n, "  ", d->tool);
        lv_label_set_text(s_session_state, session_state_text(state));
        lv_obj_set_style_text_color(s_session_state, lv_color_hex(session_color(state)), 0);
        lv_label_set_text(s_session_meta, meta);
        lv_label_set_text(s_session_summary, app_session_show_summary() ? d->summary : "");
    } else {
        lv_label_set_text(s_session_state, session_state_text(state));
        lv_obj_set_style_text_color(s_session_state, lv_color_hex(session_color(state)), 0);
        lv_label_set_text(s_session_meta, "");
        lv_label_set_text(s_session_summary, "");
    }

    if (s_session_head) {
        lv_label_set_text(s_session_head, title);
        lv_obj_set_style_text_color(s_session_head, lv_color_hex(session_color(state)), 0);
    }
}

/** 进入某个会话：绑定语音目标、请求详情、显示会话页。 */
static void open_session(const char *handle)
{
    if (!s_session_panel || !handle || !handle[0]) return;
    snprintf(s_session_handle, sizeof(s_session_handle), "%s", handle);
    const int idx = sess_find_view(handle);
    if (idx >= 0) {
        const ap_session_row_t *row = &s_sess_view[idx];
        if (s_session_head) lv_label_set_text(s_session_head, row->title[0] ? row->title : "会话");
        s_sess_cursor = idx;
    }
    s_session_detail_view.valid = false;
    lv_label_set_text(s_session_state, "");
    lv_label_set_text(s_session_meta, "");
    lv_label_set_text(s_session_summary, "");
    app_voice_set_target(handle);
    show_panel(AP_SCREEN_SESSION);
    (void)app_session_select(handle);   // 主机回 session.list（清未读）+ session.detail
    refresh_session_page();
    ESP_LOGI(TAG, "进入会话 %s", handle);
}

static void session_back_to_list(void)
{
    app_voice_set_target("");
    show_sessions_panel();
}

/** 会话列表页：上/下选择、确定进入、上长按设置、下长按余额（确定长按不动作）。 */
static bool handle_sessions_key(int btn, int event)
{
    if (event == APP_UI_EV_PRESS || event == APP_UI_EV_RELEASE || event == APP_UI_EV_DOUBLE) {
        return true;
    }
    if (event == APP_UI_EV_LONG) {
        if (btn == APP_UI_BTN_UP) {
            app_screens_hide(AP_SCREEN_SESSIONS);
            app_screens_show_settings();
        } else if (btn == APP_UI_BTN_DOWN) {
            app_screens_hide(AP_SCREEN_SESSIONS);
            app_screens_request_balance();
            (void)ap_link_send_json("{\"type\":\"" AP_MSG_BALANCE_REQ "\"}", true);
        }
        return true;
    }
    if (event != APP_UI_EV_CLICK || s_sess_count <= 0) return true;
    if (btn == APP_UI_BTN_UP) {
        s_sess_cursor = (s_sess_cursor - 1 + s_sess_count) % s_sess_count;
        refresh_sess_rows();
        return true;
    }
    if (btn == APP_UI_BTN_DOWN) {
        s_sess_cursor = (s_sess_cursor + 1) % s_sess_count;
        refresh_sess_rows();
        return true;
    }
    if (btn == APP_UI_BTN_OK) {
        open_session(s_sess_view[s_sess_cursor].handle);
        return true;
    }
    return true;
}

/** 会话页：按住确定说话、上长按返回、下长按刷新详情、上/下单击切换会话。 */
static bool handle_session_key(int btn, int event)
{
    if (btn == APP_UI_BTN_OK && event == APP_UI_EV_PRESS) {
        app_voice_start();
        return true;
    }
    if (btn == APP_UI_BTN_OK && event == APP_UI_EV_RELEASE) {
        app_voice_stop();
        return true;
    }
    if (event == APP_UI_EV_LONG) {
        if (btn == APP_UI_BTN_UP) {
            session_back_to_list();
        } else if (btn == APP_UI_BTN_DOWN) {
            app_session_request_detail();
            app_ui_post_toast("刷新中", false);
        }
        // 确定长按：PRESS 已开始录音，LONG 事件消费掉即可。
        return true;
    }
    if (event != APP_UI_EV_CLICK) return true;
    if (btn == APP_UI_BTN_UP || btn == APP_UI_BTN_DOWN) {
        if (s_sess_count <= 1) return true;
        int idx = sess_find_view(s_session_handle);
        if (idx < 0) idx = 0;
        idx = (idx + ((btn == APP_UI_BTN_UP) ? -1 : 1) + s_sess_count) % s_sess_count;
        open_session(s_sess_view[idx].handle);
    }
    return true;
}

// ── 多会话：跨任务投递与消费 ──────────────────────────────────────────────

void app_screens_post_sessions(const ap_session_row_t *rows, int count, const char *selected)
{
    if (count < 0) count = 0;
    if (count > AP_SESSION_MAX) count = AP_SESSION_MAX;
    for (int i = 0; i < count; i++) {
        s_post_sess[i] = rows[i];
    }
    s_post_sess_count = count;
    snprintf(s_post_sess_sel, sizeof(s_post_sess_sel), "%s", selected ? selected : "");
    s_sess_dirty = true;
}

void app_screens_post_session_detail(const ap_session_detail_t *detail)
{
    if (!detail) return;
    s_post_detail = *detail;
    s_detail_dirty = true;
}

void app_screens_clear_sessions(void)
{
    s_sess_count = 0;
    s_sess_dirty = false;
    s_detail_dirty = false;
    s_sess_cache_valid = false;
    s_sess_selected[0] = '\0';
    s_session_handle[0] = '\0';
    s_session_detail_view.valid = false;
    if (s_current == AP_SCREEN_SESSIONS || s_current == AP_SCREEN_SESSION) {
        if (s_sessions_panel) lv_obj_add_flag(s_sessions_panel, LV_OBJ_FLAG_HIDDEN);
        if (s_session_panel) lv_obj_add_flag(s_session_panel, LV_OBJ_FLAG_HIDDEN);
        s_current = AP_SCREEN_NONE;
    }
    app_voice_set_target("");
}

bool app_screens_handle_key(int btn, int event)
{
    if (s_current == AP_SCREEN_NONE) return false;

    // ── 多会话（协议 v2）──────────────────────────────────────────────────
    if (s_current == AP_SCREEN_SESSIONS) return handle_sessions_key(btn, event);
    if (s_current == AP_SCREEN_SESSION) return handle_session_key(btn, event);

    // ── 余额页：任意"确定"返回主页 ────────────────────────────────────────
    //
    // ★ 这里修过一个真实缺陷：原先只有审批页消费按键，余额页直接 return false
    //   把按键漏给主界面，于是用户按任何键都退不出去，而**长按确定会一路漏到
    //   主界面触发"断开蓝牙连接"** —— 用户想退出余额页，结果把链路断了。
    //   覆盖层必须吃掉所有按键，绝不能让"返回"意图穿透到有破坏性动作的层级。
    if (s_current == AP_SCREEN_BALANCE) {
        if (btn == APP_UI_BTN_OK && (event == APP_UI_EV_CLICK || event == APP_UI_EV_LONG)) {
            app_screens_hide(AP_SCREEN_BALANCE);
            s_action = AP_SCREEN_ACTION_BACK;
            ESP_LOGI(TAG, "余额页关闭（返回主页）");
        }
        return true;   // 其余按键也消费掉，避免漏到主界面
    }

    // ── 识别结果卡（docs/06 §2.2）─────────────────────────────────────────
    // 上键（单击或长按）= **删除这条结果并返回主页**（用户要求：两个手势合并）；
    // 单击确定 = 填入电脑输入框；长按确定 = 整段替换并发送（双击发送仍保留，见下）；
    // 下键单击/长按 = 向下/向上滚动查看长文本 —— 没看全就发送是最容易后悔的操作。
    if (s_current == AP_SCREEN_VOICE) {
        if (btn == APP_UI_BTN_UP && (event == APP_UI_EV_CLICK || event == APP_UI_EV_LONG)) {
            // 上键（单击或长按）= **删除这条结果并返回主页**（用户要求合并两个手势）。
            // 发 discard：主机把它真作废（删文本），之后误触「填入」只会提示已过期。
            // 想留着重填的，就别按上键 —— 卡片 30 秒无操作自动收起时是保留文本的。
            (void)send_voice_action("discard");
            app_screens_hide(AP_SCREEN_VOICE);
            ESP_LOGI(TAG, "识别结果卡：删除并返回（上键，已作废）");
            return true;
        }
        if (btn == APP_UI_BTN_DOWN && event == APP_UI_EV_CLICK) {
            lv_obj_scroll_by(s_voice_box, 0, -UI_TEXT_PITCH_CJK * 2, LV_ANIM_ON);
            s_voice_deadline_ms = now_ms() + VOICE_CARD_TTL_MS;
            return true;
        }
        if (btn == APP_UI_BTN_DOWN && event == APP_UI_EV_LONG) {
            // 长按下键 = 向上滚动（上键让给了"删除返回"）
            lv_obj_scroll_by(s_voice_box, 0, UI_TEXT_PITCH_CJK * 2, LV_ANIM_ON);
            s_voice_deadline_ms = now_ms() + VOICE_CARD_TTL_MS;
            return true;
        }
        if (btn != APP_UI_BTN_OK) return true;

        if (event == APP_UI_EV_LONG) {
            // 长按确定 = **发送**（用户要求：改成三条动作里的最后一个）。
            // 原来的"长按重说"取消 —— 想重录就按上键删除返回，再在主页按住说话。
            if (!s_voice_sent) {
                (void)send_voice_action("send");
            }
            return true;
        }
        if (event == APP_UI_EV_CLICK || event == APP_UI_EV_DOUBLE) {
            // 双击仍然发送：保留早先的"双击发送"决策，属提示栏之外的快捷方式。
            if (!s_voice_sent) {
                (void)send_voice_action(event == APP_UI_EV_DOUBLE ? "send" : "fill");
            }
            return true;
        }
        return true;   // PRESS/RELEASE 等其余事件全部消费，绝不漏到主页面的 PTT
    }

    // ── 追问 / 计划评审 / 计划阅读（docs/06 §3.2/§4）──────────────────────
    if (s_current == AP_SCREEN_QUESTION || s_current == AP_SCREEN_PLAN) {
        return handle_question_key(btn, event);
    }
    if (s_current == AP_SCREEN_Q_VOICE) {
        return handle_qvoice_key(btn, event);
    }
    // ── 系统设置页 ──
    if (s_current == AP_SCREEN_SETTINGS) {
        return handle_settings_key(btn, event);
    }
    // ── 配对码页：长按任意键关闭（配对成功后由链路侧收起）──
    if (s_current == AP_SCREEN_PAIR) {
        if (event == APP_UI_EV_LONG) {
            app_screens_hide(AP_SCREEN_PAIR);
            ESP_LOGI(TAG, "配对码页关闭");
        }
        return true;
    }
    // ── 审批页 ────────────────────────────────────────────────────────────
    // 长按确定 = 放弃本次审批。这是"我不想决定"的出口，且不会放行。
    if (btn == APP_UI_BTN_OK && event == APP_UI_EV_LONG) {
        s_appr_submitted = true;
        app_screens_hide(AP_SCREEN_APPROVAL);
        s_action = AP_SCREEN_ACTION_BACK;
        // ★ 必须同时把状态收回空闲。否则主页会停在「等待授权请求」，
        //   而审批页已经关掉 —— 用户看到一句做不到的指引，且无从恢复。
        //   （审批最终由主机侧的 30 秒超时按"拒绝"上报，这里只是让显示立刻回到一致状态。）
        app_ui_set_task_state(AP_TASK_STATE_IDLE, NULL);
        ESP_LOGI(TAG, "用户长按返回，放弃审批");
        return true;
    }
    if (event != APP_UI_EV_CLICK) return true;   // 审批页吃掉所有其它事件

    if (btn == APP_UI_BTN_UP) {
        s_appr_selected = (s_appr_selected - 1 + APPR_OPTIONS) % APPR_OPTIONS;
        refresh_approval_options();
        return true;
    }
    if (btn == APP_UI_BTN_DOWN) {
        s_appr_selected = (s_appr_selected + 1) % APPR_OPTIONS;
        refresh_approval_options();
        return true;
    }

    if (btn == APP_UI_BTN_OK) {
        const approval_option_t *opt = &s_options[s_appr_selected];
        const bool is_deny = (strcmp(opt->decision, AP_DECISION_DENY) == 0);

        // 用户决策 2026-10-06：允许/拒绝都改为**单击确认**（去掉允许类的二次确认）。
        // 光标默认停在安全方向那一项（见 refresh_approval_options），误触成本可控。
        (void)is_deny;
        send_decision(opt);
        refresh_approval_options();
        return true;
    }
    return true;
}

// ── 动作 ──────────────────────────────────────────────────────────────────

ap_screen_action_t app_screens_last_action(void) { return s_action; }

void app_screens_clear_action(void) { s_action = AP_SCREEN_ACTION_NONE; }

// ── 跨任务投递（BLE 回调里调用，只写结构，不碰 LVGL）─────────────────────

void app_screens_post_approval(const char *request_id, const char *summary,
                               const char *session_title, bool high_risk, uint32_t timeout_ms)
{
    snprintf(s_post_appr.request_id, sizeof(s_post_appr.request_id), "%s", request_id ? request_id : "");
    snprintf(s_post_appr.summary, sizeof(s_post_appr.summary), "%s", summary ? summary : "");
    snprintf(s_post_appr.session_title, sizeof(s_post_appr.session_title), "%s",
             session_title ? session_title : "");
    s_post_appr.high_risk = high_risk;
    s_post_appr.timeout_ms = timeout_ms;
    s_appr_dirty = true;
}

void app_screens_post_approval_done(const char *request_id, const char *outcome)
{
    snprintf(s_appr_done_id, sizeof(s_appr_done_id), "%s", request_id ? request_id : "");
    (void)outcome;
    s_appr_done_dirty = true;
}

void app_screens_post_balance(const char *total, const char *currency,
                              const char *recharge, const char *bonus, const char *today_used)
{
    snprintf(s_post_balance.total, sizeof(s_post_balance.total), "%s", total ? total : "");
    snprintf(s_post_balance.currency, sizeof(s_post_balance.currency), "%s", currency ? currency : "");
    snprintf(s_post_balance.recharge, sizeof(s_post_balance.recharge), "%s", recharge ? recharge : "");
    snprintf(s_post_balance.bonus, sizeof(s_post_balance.bonus), "%s", bonus ? bonus : "");
    snprintf(s_post_balance.today_used, sizeof(s_post_balance.today_used), "%s", today_used ? today_used : "");
    s_balance_dirty = true;
}

void app_screens_post_voice_result(const char *result_id, const char *text, const char *target_handle)
{
    snprintf(s_post_voice.result_id, sizeof(s_post_voice.result_id), "%s", result_id ? result_id : "");
    snprintf(s_post_voice.text, sizeof(s_post_voice.text), "%s", text ? text : "");
    snprintf(s_post_voice.target, sizeof(s_post_voice.target), "%s", target_handle ? target_handle : "");
    s_voice_dirty = true;
}

void app_screens_post_question(const ap_question_msg_t *q)
{
    if (!q) return;
    s_post_q = *q;
    s_q_dirty = true;
}

void app_screens_post_question_done(const char *call_id, bool ok, const char *reason)
{
    snprintf(s_post_q_done.call_id, sizeof(s_post_q_done.call_id), "%s", call_id ? call_id : "");
    s_post_q_done.ok = ok;
    snprintf(s_post_q_done.reason, sizeof(s_post_q_done.reason), "%s", reason ? reason : "");
    s_q_done_dirty = true;
}

// ── 消费投递（由 app_ui 的 LVGL 定时器调用）──────────────────────────────

void app_screens_consume_pending(void);

void app_screens_consume_pending(void)
{
    // 会话列表：协议 v2 的"主页"。列表到达时若没有更重要的页面，直接显示。
    if (s_sess_dirty) {
        s_sess_dirty = false;
        s_sess_count = s_post_sess_count;
        for (int i = 0; i < s_sess_count; i++) s_sess_view[i] = s_post_sess[i];
        snprintf(s_sess_selected, sizeof(s_sess_selected), "%s", s_post_sess_sel);
        s_sess_cache_valid = false;
        if (s_sess_count > 0) {
            // 正在看的会话被关闭/移出列表 → 回列表，并清掉语音目标。
            if (s_current == AP_SCREEN_SESSION && sess_find_view(s_session_handle) < 0) {
                app_voice_set_target("");
                show_sessions_panel();
            }
            if (s_current == AP_SCREEN_NONE || s_current == AP_SCREEN_SESSIONS) {
                show_sessions_panel();
            } else {
                refresh_sess_rows();
            }
        } else {
            // 列表清空（主机断开/无会话）：退回四态主页。
            if (s_current == AP_SCREEN_SESSIONS || s_current == AP_SCREEN_SESSION) {
                app_voice_set_target("");
                app_screens_clear_sessions();
            }
        }
        ESP_LOGI(TAG, "会话列表更新：%d 条（选中 %s）", s_sess_count,
                 s_sess_selected[0] ? s_sess_selected : "-");
    }

    if (s_detail_dirty) {
        s_detail_dirty = false;
        s_session_detail_view = s_post_detail;
        if (s_current == AP_SCREEN_SESSION &&
            strcmp(s_session_detail_view.handle, s_session_handle) == 0) {
            refresh_session_page();
        }
    }

    // 配对码页：未配对的主机接入时弹出（优先级排在审批之后，见下面的顺序）
    if (s_pair_dirty) {
        s_pair_dirty = false;
        (void)s_post_pair_code;
        (void)s_post_pair_id;
        refresh_pair_page();
        // 只有"没有更重要的页面"时才抢占：审批/追问/结果卡都是用户正在处理的东西
        if (s_current == AP_SCREEN_NONE || s_current == AP_SCREEN_SETTINGS ||
            s_current == AP_SCREEN_PAIR) {
            show_panel(AP_SCREEN_PAIR);
            ESP_LOGI(TAG, "未配对：弹出配对码页（码见屏幕）");
        }
    }

    // 审批优先：它可能随时到达，且必须立刻可见
    if (s_appr_dirty) {
        s_appr_dirty = false;
        app_screens_show_approval(s_post_appr.request_id, s_post_appr.summary,
                                  s_post_appr.session_title,
                                  s_post_appr.high_risk, s_post_appr.timeout_ms);
    }

    if (s_appr_done_dirty) {
        s_appr_done_dirty = false;
        // 只收起**同一个请求**的审批页，避免误关刚到达的新请求
        if (s_current == AP_SCREEN_APPROVAL && strcmp(s_appr_done_id, s_appr_id) == 0) {
            app_screens_hide(AP_SCREEN_APPROVAL);
        }
    }

    // 追问/计划评审：审批之后处理 —— 审批永远最先（它在等一个更关键的决定）。
    if (s_q_done_dirty) {
        s_q_done_dirty = false;
        const bool q_open = (s_current == AP_SCREEN_QUESTION || s_current == AP_SCREEN_PLAN ||
                             s_current == AP_SCREEN_Q_VOICE);
        if (q_open && strcmp(s_post_q_done.call_id, s_q.call_id) == 0) {
            if (s_post_q_done.ok) {
                q_finish("已提交", false);
            } else if (strcmp(s_post_q_done.reason, "timeout") == 0) {
                q_finish("已超时，请到电脑上继续", false);
            } else {
                q_finish("问题已结束", false);
            }
        }
    }
    // ★ question.req 期间不抢审批屏；其余场景直接显示/换题
    //   （question.nav 的回包就是"下一题"的 question.req）。
    if (s_q_dirty && s_current != AP_SCREEN_APPROVAL) {
        s_q_dirty = false;
        show_question(&s_post_q);
    }

    // 识别结果卡：审批/追问之后、余额之前。
    // ★ 更高的覆盖页显示期间**不抢屏**：留着脏标志，等它们收起后的下一轮再弹。
    if (s_voice_dirty && s_current != AP_SCREEN_APPROVAL && s_current != AP_SCREEN_QUESTION &&
        s_current != AP_SCREEN_PLAN && s_current != AP_SCREEN_Q_VOICE) {
        s_voice_dirty = false;
        snprintf(s_voice_target, sizeof(s_voice_target), "%s", s_post_voice.target);
        app_screens_show_voice_result(s_post_voice.result_id, s_post_voice.text);
    }

    if (s_balance_dirty) {
        s_balance_dirty = false;
        app_screens_show_balance(s_post_balance.total, s_post_balance.currency,
                                 s_post_balance.recharge, s_post_balance.bonus,
                                 s_post_balance.today_used);
    }
}
