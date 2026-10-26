// tests/test_session_logic.c —— 多会话列表/详情纯逻辑的 host 测试（协议 v2）。
//
// 这些错误在真机上的表现都很"像 BLE 故障"，因此必须在这里用精确断言钉死：
//   · 扁平字段解码错位 → 设备显示成别人的标题（A 的标题挂在 B 行）；
//   · 尾部空标题被吞 → 最后一条会话的标题残留为空/上一条的旧值；
//   · handle 用错/重复 → 选中后详情回到别的会话；
//   · 未读/状态收敛错 → 用户以为任务还在跑，或已完成却一直红点。
#include "app_session_logic.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void expect_str(const char *got, const char *want, const char *label)
{
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "✗ %s：\n  期望 [%s]\n  实际 [%s]\n", label, want, got);
        assert(0);
    }
}

static void test_state_mapping(void)
{
    uint8_t st = 0;
    assert(ap_session_state_parse(AP_SSTATE_RUNNING, &st));
    assert(st == AP_SESSION_STATE_RUNNING);
    assert(ap_session_state_parse(AP_SSTATE_WAITING, &st));
    assert(st == AP_SESSION_STATE_WAITING);
    assert(ap_session_state_parse(AP_SSTATE_ERROR, &st));
    assert(st == AP_SESSION_STATE_ERROR);
    assert(ap_session_state_parse(AP_SSTATE_ABORTED, &st));
    assert(st == AP_SESSION_STATE_ABORTED);
    assert(ap_session_state_parse(AP_SSTATE_IDLE, &st));
    assert(st == AP_SESSION_STATE_IDLE);
    assert(!ap_session_state_parse("bogus", &st));
    assert(!ap_session_state_parse("", &st));
    expect_str(ap_session_state_name(AP_SESSION_STATE_RUNNING), "running", "state name");
}

static void test_apply_normal(void)
{
    ap_session_list_t list;
    bool ok = ap_session_list_apply(&list,
                                    "01|02",
                                    "重构登录|调研向量库",
                                    "running|waiting",
                                    "0|1",
                                    "02");
    assert(ok);
    assert(list.count == 2);
    expect_str(list.rows[0].handle, "01", "row0 handle");
    expect_str(list.rows[0].title, "重构登录", "row0 title");
    assert(list.rows[0].state == AP_SESSION_STATE_RUNNING);
    assert(!list.rows[0].unread);
    expect_str(list.rows[1].title, "调研向量库", "row1 title");
    assert(list.rows[1].state == AP_SESSION_STATE_WAITING);
    assert(list.rows[1].unread);
    expect_str(list.selected, "02", "选中回显");
    const ap_session_row_t *sel = ap_session_selected_row(&list);
    assert(sel && strcmp(sel->handle, "02") == 0);
}

static void test_apply_empty(void)
{
    ap_session_list_t list;
    ap_session_list_apply(&list, "01", "旧值", "running", "0", "01");
    assert(ap_session_list_apply(&list, "", "", "", "", ""));
    assert(list.count == 0);
    expect_str(list.selected, "", "空列表选中应为空");
    assert(ap_session_selected_row(&list) == NULL);
}

static void test_apply_trailing_empty_title(void)
{
    // 主机 join 后末尾为空会变成 "A|"：ap_flat_next 到结尾只迭代 1 次，
    // 但 handles 的 n=2，第二行必须补成空串而不是残留。
    ap_session_list_t list;
    assert(ap_session_list_apply(&list, "01|02", "A|", "running|idle", "0|0", ""));
    assert(list.count == 2);
    expect_str(list.rows[0].title, "A", "第一行标题");
    expect_str(list.rows[1].title, "", "尾部空标题必须补空");

    // 两个都为空：titles = "|"，同样要得到 2 个空标题。
    assert(ap_session_list_apply(&list, "01|02", "|", "idle|idle", "0|0", ""));
    assert(list.count == 2);
    expect_str(list.rows[0].title, "", "全空标题 1");
    expect_str(list.rows[1].title, "", "全空标题 2");
}

static void test_apply_escapes(void)
{
    // 主机把字段内 '|' 转义成 U+00A6、'\' 转义成 U+2572；设备解码必须还原。
    ap_session_list_t list;
    assert(ap_session_list_apply(&list,
                                 "01|02",
                                 "a" "\xC2\xA6" "b|c" "\xE2\x95\xB2" "d",
                                 "idle|idle",
                                 "0|0",
                                 ""));
    assert(list.count == 2);
    expect_str(list.rows[0].title, "a|b", "¦ 还原为 |");
    expect_str(list.rows[1].title, "c" "\\" "d", "╲ 还原为 \\");
}

static void test_apply_max_and_over(void)
{
    ap_session_list_t list;
    assert(ap_session_list_apply(&list, "01|02|03|04|05|06|07|08",
                                 "T1|T2|T3|T4|T5|T6|T7|T8",
                                 "idle|idle|idle|idle|idle|idle|idle|idle",
                                 "0|0|0|0|0|0|0|0", ""));
    assert(list.count == AP_SESSION_MAX);

    // 9 条：超上限必须失败并清空（避免半旧半新）。
    assert(!ap_session_list_apply(&list, "01|02|03|04|05|06|07|08|09",
                                  "T1|T2|T3|T4|T5|T6|T7|T8|T9",
                                  "idle|idle|idle|idle|idle|idle|idle|idle|idle",
                                  "0|0|0|0|0|0|0|0|0", "01"));
    assert(list.count == 0);
}

static void test_apply_skips_blank_and_duplicate_handles(void)
{
    ap_session_list_t list;
    assert(ap_session_list_apply(&list, "01||01|02", "A||dup|B", "idle|idle|idle|idle",
                                 "0|0|0|0", ""));
    assert(list.count == 2);
    expect_str(list.rows[0].title, "A", "第一行");
    expect_str(list.rows[1].title, "B", "重复 handle 后的行");
}

static void test_utf8_safe_truncation(void)
{
    // 14 个汉字 = 42 字节 > 40 字节上限（AP_SESSION_TITLE_BYTES）。
    // 必须截到 13 个完整汉字（39 字节），而不是切出半个字。
    const char *title = "一二三四五六七八九十一二三四";
    ap_session_list_t list;
    assert(ap_session_list_apply(&list, "01", title, "idle", "0", ""));
    const size_t len = strlen(list.rows[0].title);
    assert(len == 39);
    int chars = 0;
    for (size_t i = 0; i < len; i++) {
        if ((((unsigned char)list.rows[0].title[i]) & 0xC0) != 0x80) chars++;
    }
    assert(chars == 13);
}

static void test_select_semantics(void)
{
    ap_session_list_t list;
    ap_session_list_apply(&list, "01|02", "A|B", "idle|idle", "0|0", "");
    assert(!ap_session_set_selected(&list, "03"));
    expect_str(list.selected, "", "不存在的 handle 应清空选中");
    assert(ap_session_set_selected(&list, "02"));
    expect_str(list.selected, "02", "正常选中");
    ap_session_clear_selected(&list);
    expect_str(list.selected, "", "清空选中");
}

static void test_done_convergence(void)
{
    ap_session_list_t list;
    ap_session_list_apply(&list, "01|02", "A|B", "running|running", "0|0", "01");

    ap_session_apply_done(&list, "02", "error");
    assert(list.rows[1].state == AP_SESSION_STATE_ERROR);
    assert(list.rows[1].unread);   // 未选中会话结束后应置未读

    ap_session_apply_done(&list, "01", "completed");
    assert(list.rows[0].state == AP_SESSION_STATE_IDLE);
    assert(!list.rows[0].unread);  // 选中的会话结束后不置未读

    ap_session_apply_done(&list, "02", "aborted");
    assert(list.rows[1].state == AP_SESSION_STATE_ABORTED);

    // 不存在的 handle 是安全空操作
    ap_session_apply_done(&list, "ff", "error");
}

static void test_detail(void)
{
    ap_session_detail_t d;
    ap_session_detail_clear(&d);
    assert(!d.valid);
    assert(d.step == -1);

    ap_session_detail_apply(&d, "02", "调研向量库", "running", 3, 7, "grep", 12345, "最近输出");
    assert(d.valid);
    expect_str(d.handle, "02", "detail handle");
    expect_str(d.title, "调研向量库", "detail title");
    assert(d.state == AP_SESSION_STATE_RUNNING);
    assert(d.step == 3 && d.step_total == 7);
    expect_str(d.tool, "grep", "detail tool");
    assert(d.elapsed_ms == 12345);
    expect_str(d.summary, "最近输出", "detail summary");

    ap_session_detail_apply(&d, "", "", "", 0, 0, "", 0, "");
    assert(!d.valid);              // 空 handle 的详情应失效

    ap_session_detail_apply(&d, "01", "A", "bogus", -1, 0, "", 0, "");
    assert(d.valid);
    assert(d.state == AP_SESSION_STATE_UNKNOWN);
    assert(d.step == -1);
}

int main(void)
{
    test_state_mapping();
    test_apply_normal();
    test_apply_empty();
    test_apply_trailing_empty_title();
    test_apply_escapes();
    test_apply_max_and_over();
    test_apply_skips_blank_and_duplicate_handles();
    test_utf8_safe_truncation();
    test_select_semantics();
    test_done_convergence();
    test_detail();
    printf("test_session_logic: PASS\n");
    return 0;
}
