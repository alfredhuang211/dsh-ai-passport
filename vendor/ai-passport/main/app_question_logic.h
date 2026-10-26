// 追问 / 计划评审的纯逻辑接口（docs/06 §3/§4）。
//
// 与 app_proto_logic.h 同一个理由拆出来：只放**纯逻辑**，
// host 侧测试只 include 这一个（不牵进 NimBLE / LVGL / FreeRTOS）。
//
// 这里集中做三类最容易出错、且错误在真机上极难定位的事：
//   1. 选项串拆分（'|' 分隔 → 定长标签数组）；
//   2. 选择/光标语义（单选互斥、多选翻转、光标循环）；
//   3. 出站 JSON 组装（含转义 —— custom 文本里的引号会把整条 JSON 打断，
//      表现为"带引号的补充答案发不出去"，看起来像 BLE 故障）。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AP_Q_LABEL_SIZE 40
#define AP_Q_MAX_OPTIONS 5

// '|' 分隔的选项串拆成定长标签数组（最多 max 个）。
// 返回拆出的标签数；csv 为 NULL/空返回 0。
int ap_q_split_options(const char *csv, char labels[][AP_Q_LABEL_SIZE], int max);

// 光标在 rows 行之间循环移动（delta 可正可负，可跨多行）。
// rows <= 0 时返回 0。
int ap_q_cursor_move(int cursor, int rows, int delta);

// 单选：只保留 index 选中（越界则全清）。多选：翻转 index（越界不动）。
void ap_q_select_single(bool *selected, int count, int index);
void ap_q_select_toggle(bool *selected, int count, int index);

// JSON 字符串转义（'\' 与 '"' 与控制字符）。输出永远以 '\0' 结尾；
// 缓冲不够时在**完整转义序列**边界截断（不产出半截 \u 序列）。
// 返回写入的字符数（不含结尾 0）。
size_t ap_json_escape(char *out, size_t out_size, const char *src);

// ── 出站消息组装（返回 snprintf 语义；发送上限由链路层拦截）──────────────

// question.pick：{type,callId,qid,selected:[…],custom,skipped}
int ap_q_build_pick_json(char *out, size_t out_size, const char *call_id, const char *qid,
                         const char labels[][AP_Q_LABEL_SIZE], const bool *selected, int count,
                         const char *custom, bool skipped);

// question.answer：{type,callId,action}（action = submit / cancel / reject）
int ap_q_build_answer_json(char *out, size_t out_size, const char *call_id, const char *action);

// question.nav：{type,callId,index}
int ap_q_build_nav_json(char *out, size_t out_size, const char *call_id, int index);

// ── 行模型（docs/06 §3/§4）：选项行 + 其他行 + [下一题]行 + 跳过行 ─────────
//
// 计划评审页固定 2 行（要求修改 / 同意执行）；选择题行数 =
// 选项 + 其他 + [下一题（非末题才有）] + 跳过。
// 这些行号与行文本是**界面层增量刷新**的对比基准（只重绘变了的行），
// 也决定"光标走到哪一行会发生什么" —— 全部是纯逻辑，host 测试钉死。
#define AP_Q_ROW_TEXT_MAX 256

int ap_q_row_count(bool is_plan, int option_count, int index, int total);
int ap_q_row_custom(bool is_plan, int option_count);
bool ap_q_row_has_next(bool is_plan, int index, int total);
int ap_q_row_next(bool is_plan, int option_count, int index, int total);
int ap_q_row_skip(bool is_plan, int option_count, int index, int total);

// 行显示文本（返回 snprintf 语义）。选项行带选中标记：
// 多选 [x]/[ ]，单选 (*)/( )；custom 行显示已录入文本或"语音输入"占位。
// row 越界输出空串。
int ap_q_row_text(char *out, size_t out_size,
                  bool is_plan, bool multi_select,
                  const bool *selected, const char labels[][AP_Q_LABEL_SIZE],
                  int option_count, int index, int total,
                  const char *custom, int row);

#ifdef __cplusplus
}
#endif
