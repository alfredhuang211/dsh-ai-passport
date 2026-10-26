// main/app_screens.h —— 任务详情 / 余额 / 审批三块界面。
//
// 为什么单独拆一个模块而不是塞进 app_ui.c：
//   这三块都是**覆盖式**界面（弹出时盖住主页，任务台状态照旧保留），
//   生命周期与主页不同；混在一起会让 app_ui.c 的状态机迅速变复杂。
//
// 线程约束与 app_ui 一致：所有函数必须在 LVGL 任务里或持有 bsp_lvgl_lock() 时调用；
// 唯一例外是 app_screens_post_* 系列，它们只投递数据，由 LVGL 定时器消费。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_link.h"
#include "app_session_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AP_SCREEN_NONE = 0,  // 只显示主页
    AP_SCREEN_BALANCE,   // 余额
    AP_SCREEN_VOICE,     // 语音识别结果卡（单击填入 / 双击发送 / 按住重说）
    AP_SCREEN_QUESTION,  // 追问（选项行 + 自定义行 + 跳过）
    AP_SCREEN_Q_VOICE,   // 自定义答案的语音输入页（可放弃返回）
    AP_SCREEN_PLAN,      // 计划评审（要求修改 / 批准计划）
    AP_SCREEN_APPROVAL,  // 审批（优先级最高，会盖住上面所有）
    AP_SCREEN_SESSIONS,  // 会话列表（协议 v2 主页：上下选择 / 确定进入）
    AP_SCREEN_SESSION,   // 会话详情（按住确定说话 / 上长按返回）
    AP_SCREEN_SETTINGS,  // 系统设置（主页长按上键进入）：熄屏 / 亮度 / 自动休眠 / 提示音量
    AP_SCREEN_PAIR,      // 配对码（未配对的主机接入时弹出；每次上电换码）
} ap_screen_t;

// ── 追问/计划评审的下行消息（app.c 解析后投递，docs/06 §3/§4）──────────────

#define AP_QUESTION_ID_MAX 48
#define AP_QUESTION_SUMMARY_MAX 128
#define AP_QUESTION_LABEL_MAX 40
#define AP_QUESTION_OPTS_CAP (AP_QUESTION_LABEL_MAX * 5 + 8)

typedef struct {
    char call_id[AP_QUESTION_ID_MAX];
    char qid[AP_QUESTION_ID_MAX];
    // ★ 只有一行极短摘要：完整题干/选项/计划正文都留在电脑卡片上（用户决策
    //   2026-10-06 —— 小屏读长文本不现实，设备退化成"实体遥控器"）。
    char summary[AP_QUESTION_SUMMARY_MAX];
    char approve_label[AP_QUESTION_LABEL_MAX];
    char options_csv[AP_QUESTION_OPTS_CAP];   // '|' 分隔（设备端不解析嵌套数组）
    int recommended_index;
    int index;                                // 0 起
    int total;
    bool multi_select;
    bool is_plan;                             // kind == "plan"
    long timeout_ms;
} ap_question_msg_t;

// 创建全部覆盖层（在 app_ui_create 之后调用）。
void app_screens_create(void);

// 销毁覆盖层。
void app_screens_destroy(void);

// 当前覆盖层状态。app_main 用它决定长按确定是"返回"还是"断开"。
ap_screen_t app_screens_current(void);

// 是否有待处理的审批（审批页正在显示）。
bool app_screens_approval_active(void);

// ── 显示与隐藏（LVGL 任务内调用）──────────────────────────────────────────

// 显示任务详情。字段可为 NULL。
// 显示余额。amounts 为已格式化好的字符串（避免在设备上做浮点解析）。
void app_screens_show_balance(const char *total, const char *currency,
                              const char *recharge, const char *bonus, const char *today_used);

// 显示审批请求：只有一行极短摘要（正文在电脑卡片上看）。
// 返回后由 UI 定时器驱动倒计时。
void app_screens_show_approval(const char *request_id, const char *summary,
                               const char *session_title, bool high_risk, uint32_t timeout_ms);

// 收起指定的覆盖层。传 AP_SCREEN_NONE 收起全部。
void app_screens_hide(ap_screen_t which);

/** 打开系统设置页（主页长按上键）。 */
void app_screens_show_settings(void);

/** 弹出配对码页（未配对的主机接入时；BLE 任务调用，内部走跨任务投递）。 */
void app_screens_post_pair_code(const char *code, const char *device_id);

// ── 定时器驱动（LVGL 定时器内调用）────────────────────────────────────────

// 刷新识别结果卡的自动收起计时（30 秒无操作收起，文本留在主机语音列表）。
void app_screens_tick_voice(uint32_t now_ms);

// 刷新追问/计划评审的倒计时条（真正的超时由主机裁决并回 question.done）。
void app_screens_tick_question(uint32_t now_ms);

// ── 按键（LVGL 任务内调用）───────────────────────────────────────────────

// 返回 true 表示事件已被覆盖层消费（主页不应再处理）。
bool app_screens_handle_key(int btn, int event);

// ── 跨任务投递（线程安全）────────────────────────────────────────────────

// 投递"显示审批"（只有一行极短摘要）。审批由 BLE 回调触发，不能直接碰 LVGL。
void app_screens_post_approval(const char *request_id, const char *summary,
                               const char *session_title, bool high_risk, uint32_t timeout_ms);

// 投递"审批已结束"（无论是谁决定的），用来收起页面。
void app_screens_post_approval_done(const char *request_id, const char *outcome);

// 投递余额。
void app_screens_post_balance(const char *total, const char *currency,
                              const char *recharge, const char *bonus, const char *today_used);

// 用户长按下键请求余额：下一条余额数据到达时才弹出余额页（主动推送不抢屏幕）。
void app_screens_request_balance(void);

// 投递语音识别结果（识别结果卡，docs/06 §2）。BLE 回调里调用，只写结构。
// target_handle：协议 v2 录音目标会话（空串 = 未指定）；结果卡的发送动作会带上它，
// 避免"在 A 会话录的音，发到后来切换到的 B 会话"。
void app_screens_post_voice_result(const char *result_id, const char *text, const char *target_handle);

// 投递追问/计划评审（docs/06 §3/§4）。BLE 回调里调用，只写结构。
void app_screens_post_question(const ap_question_msg_t *q);
// 投递"本批问题已结束"（提交被采纳 / 超时 / 已在电脑上回答），用来收起页面。
void app_screens_post_question_done(const char *call_id, bool ok, const char *reason);
// 追问页的 custom 语音录入结果（docs/06 §3.3）。
// 返回 true 表示已被追问页消费（不走识别结果卡）；追问页不在时丢弃并提示。
bool app_question_on_voice_result(const char *text);

// ── 多会话（协议 v2）：跨任务投递，LVGL 定时器消费 ─────────────────────

// 整表替换会话列表（app 任务调用；只拷贝数据，不碰 LVGL）。
void app_screens_post_sessions(const ap_session_row_t *rows, int count, const char *selected);

// 投递会话详情（仅当正在看该会话时应用）。
void app_screens_post_session_detail(const ap_session_detail_t *detail);

/**
 * 清空会话视图并收起列表/详情页（LVGL 任务内调用）。
 * 链路断开时用：避免把"上次连接时的会话"当成在线会话继续展示。
 */
void app_screens_clear_sessions(void);

// 返回值为 true 表示"用户想返回"：由 app.c 把选中的任务 id 记录下来并请求详情。
// 审批页需要在按键时把决定发回主机，因此它自己发消息，不走这个返回值。
typedef enum {
    AP_SCREEN_ACTION_NONE = 0,
    AP_SCREEN_ACTION_BACK,       // 用户要求返回主页
} ap_screen_action_t;

ap_screen_action_t app_screens_last_action(void);
void app_screens_clear_action(void);

#ifdef __cplusplus
}
#endif
