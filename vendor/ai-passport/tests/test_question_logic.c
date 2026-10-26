// tests/test_question_logic.c —— 追问/计划评审纯逻辑的 host 测试（docs/06 §3/§4）。
//
// 为什么需要它：这里全是"错了在真机上才发作"的逻辑 ——
//   · 跳着多选漏逗号 → 非法 JSON → "跳着勾就发不出去"，看起来像 BLE 故障；
//   · custom 文本里的引号没转义 → 整条消息断掉，表现为"补充答案发不出去"；
//   · 单选没互斥 → 答案里出现两个选项，主机钳制后与屏幕显示不一致。
// 每一个都用精确输出断言钉死。
#include "app_question_logic.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void expect_str(const char *got, const char *want, const char *label)
{
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "✗ %s：\n  期望 %s\n  实际 %s\n", label, want, got);
        assert(0);
    }
}

static void test_split_options(void)
{
    char labels[AP_Q_MAX_OPTIONS][AP_Q_LABEL_SIZE];

    int n = ap_q_split_options("macOS|Windows|Linux", labels, AP_Q_MAX_OPTIONS);
    assert(n == 3);
    expect_str(labels[0], "macOS", "split 第 1 项");
    expect_str(labels[2], "Linux", "split 第 3 项");

    // 空段不占选项位
    n = ap_q_split_options("a||b", labels, AP_Q_MAX_OPTIONS);
    assert(n == 2);
    expect_str(labels[1], "b", "空段被跳过");

    // 超过 max 只取前 max 个
    n = ap_q_split_options("1|2|3|4|5|6", labels, AP_Q_MAX_OPTIONS);
    assert(n == AP_Q_MAX_OPTIONS);
    expect_str(labels[4], "5", "超出上限截断");

    // NULL / 空串
    assert(ap_q_split_options(NULL, labels, AP_Q_MAX_OPTIONS) == 0);
    assert(ap_q_split_options("", labels, AP_Q_MAX_OPTIONS) == 0);
}

static void test_cursor_move(void)
{
    assert((ap_q_cursor_move(0, 5, -1) == 4) && "向上回绕");
    assert((ap_q_cursor_move(4, 5, 1) == 0) && "向下回绕");
    assert((ap_q_cursor_move(2, 5, 3) == 0) && "跨多行");
    assert((ap_q_cursor_move(0, 0, 1) == 0) && "无行时恒为 0");
}

static void test_select(void)
{
    bool sel[3] = { true, true, false };

    ap_q_select_single(sel, 3, 1);
    assert((!sel[0] && sel[1] && !sel[2]) && "单选互斥：只留 index");

    ap_q_select_single(sel, 3, 99);
    assert((!sel[0] && !sel[1] && !sel[2]) && "单选越界：全清");

    ap_q_select_toggle(sel, 3, 2);
    assert((sel[2]) && "多选翻转：选中");
    ap_q_select_toggle(sel, 3, 2);
    assert((!sel[2]) && "多选翻转：取消");
    ap_q_select_toggle(sel, 3, -1);   // 越界不动，不许崩
}

static void test_escape(void)
{
    char out[64];

    ap_json_escape(out, sizeof(out), "ab\"c\\d");
    expect_str(out, "ab\\\"c\\\\d", "引号与反斜杠转义");

    ap_json_escape(out, sizeof(out), "行\n换行");
    expect_str(out, "行\\u000a换行", "控制字符走 \\u 序列");

    // 缓冲不足：在完整转义序列前停下，不产出半截序列
    char tiny[4];
    size_t n = ap_json_escape(tiny, sizeof(tiny), "\"\"\"");
    assert((n == 2 && tiny[0] == '\\' && tiny[1] == '"') && "截断落在完整转义序列边界");
}

static void test_pick_json(void)
{
    char out[256];
    char labels[AP_Q_MAX_OPTIONS][AP_Q_LABEL_SIZE] = { "macOS", "Windows", "Linux" };
    bool sel[3] = { true, false, true };

    ap_q_build_pick_json(out, sizeof(out), "call-1", "q1", labels, sel, 3, "", false);
    expect_str(out,
               "{\"type\":\"question.pick\",\"callId\":\"call-1\",\"qid\":\"q1\","
               "\"selected\":[\"macOS\",\"Linux\"],\"custom\":\"\",\"skipped\":0}",
               "跳着多选的逗号不能漏");

    // 只选中间一项
    bool mid[3] = { false, true, false };
    ap_q_build_pick_json(out, sizeof(out), "call-1", "q1", labels, mid, 3, "还要 Android", false);
    expect_str(out,
               "{\"type\":\"question.pick\",\"callId\":\"call-1\",\"qid\":\"q1\","
               "\"selected\":[\"Windows\"],\"custom\":\"还要 Android\",\"skipped\":0}",
               "单项选择 + custom");

    // custom 里的引号必须转义，否则整条 JSON 断掉
    bool none[3] = { false, false, false };
    ap_q_build_pick_json(out, sizeof(out), "call-1", "q1", labels, none, 3, "说 \"好\"", true);
    expect_str(out,
               "{\"type\":\"question.pick\",\"callId\":\"call-1\",\"qid\":\"q1\","
               "\"selected\":[],\"custom\":\"说 \\\"好\\\"\",\"skipped\":1}",
               "custom 转义 + skipped");
}

static void test_action_json(void)
{
    char out[128];

    ap_q_build_answer_json(out, sizeof(out), "call-9", "reject");
    expect_str(out, "{\"type\":\"question.answer\",\"callId\":\"call-9\",\"action\":\"reject\"}",
               "answer（要求修改 = reject）");

    ap_q_build_nav_json(out, sizeof(out), "call-9", 2);
    expect_str(out, "{\"type\":\"question.nav\",\"callId\":\"call-9\",\"index\":2}", "nav");
}

static void test_row_model(void)
{
    // 计划评审固定 2 行；没有 custom / 下一题
    assert((ap_q_row_count(true, 3, 0, 1) == 2) && "计划页 2 行");
    assert((ap_q_row_custom(true, 3) == -1) && "计划页没有 custom 行");
    assert(!ap_q_row_has_next(true, 0, 5) && "计划页没有下一题");

    // 选择题非末题：选项 + custom + 下一题 + 跳过
    assert((ap_q_row_count(false, 3, 0, 2) == 6) && "非末题行数 = 3+2+1");
    assert((ap_q_row_custom(false, 3) == 3) && "custom 行号");
    assert((ap_q_row_next(false, 3, 0, 2) == 4) && "下一题行号");
    assert((ap_q_row_skip(false, 3, 0, 2) == 5) && "跳过行 = 最后一行");

    // 末题：没有下一题行
    assert((ap_q_row_count(false, 3, 1, 2) == 5) && "末题行数 = 3+2");
    assert(!ap_q_row_has_next(false, 1, 2) && "末题没有下一题");
    assert((ap_q_row_next(false, 3, 1, 2) == -1) && "末题下一题行号 = -1");
    assert((ap_q_row_skip(false, 3, 1, 2) == 4) && "末题跳过行 = 最后一行");
}

static void test_row_text(void)
{
    char out[AP_Q_ROW_TEXT_MAX];
    char labels[AP_Q_MAX_OPTIONS][AP_Q_LABEL_SIZE] = { "macOS", "Windows", "Linux" };
    bool sel[3] = { true, false, false };

    // 计划页两行
    ap_q_row_text(out, sizeof(out), true, false, sel, labels, 3, 0, 1, "", 0);
    expect_str(out, "要求修改", "计划页第 0 行");
    ap_q_row_text(out, sizeof(out), true, false, sel, labels, 3, 0, 1, "", 1);
    expect_str(out, "同意执行", "计划页第 1 行");

    // 选项行标记：单选 (*)/( )，多选 [x]/[ ]
    ap_q_row_text(out, sizeof(out), false, false, sel, labels, 3, 0, 2, "", 0);
    expect_str(out, "(*) macOS", "单选选中标记");
    ap_q_row_text(out, sizeof(out), false, false, sel, labels, 3, 0, 2, "", 1);
    expect_str(out, "( ) Windows", "单选未选标记");
    ap_q_row_text(out, sizeof(out), false, true, sel, labels, 3, 0, 2, "", 0);
    expect_str(out, "[x] macOS", "多选勾选标记");
    ap_q_row_text(out, sizeof(out), false, true, sel, labels, 3, 0, 2, "", 2);
    expect_str(out, "[ ] Linux", "多选未勾标记");

    // custom 行：有文本显文本，没文本显占位
    ap_q_row_text(out, sizeof(out), false, false, sel, labels, 3, 0, 2, "还要 Android", 3);
    expect_str(out, "其他：还要 Android", "custom 行显示录入文本");
    ap_q_row_text(out, sizeof(out), false, false, sel, labels, 3, 0, 2, "", 3);
    expect_str(out, "其他：语音输入", "custom 行占位");

    // 动作行
    ap_q_row_text(out, sizeof(out), false, false, sel, labels, 3, 0, 2, "", 4);
    expect_str(out, "下一题", "下一题行");
    ap_q_row_text(out, sizeof(out), false, false, sel, labels, 3, 0, 2, "", 5);
    expect_str(out, "跳过此题", "跳过行");
    ap_q_row_text(out, sizeof(out), false, false, sel, labels, 3, 1, 2, "", 4);
    expect_str(out, "跳过此题", "末题最后一行是跳过");

    // 越界行：输出空串，不许崩
    ap_q_row_text(out, sizeof(out), false, false, sel, labels, 3, 0, 2, "", 6);
    expect_str(out, "", "越界行为空");
    ap_q_row_text(out, sizeof(out), false, false, sel, labels, 3, 0, 2, "", -1);
    expect_str(out, "", "负行号为空");
}

int main(void)
{
    test_split_options();
    test_cursor_move();
    test_select();
    test_escape();
    test_pick_json();
    test_action_json();
    test_row_model();
    test_row_text();
    printf("== question logic 测试全部通过 ==\n");
    return 0;
}
