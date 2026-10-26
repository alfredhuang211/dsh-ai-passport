#!/usr/bin/env python3
"""LVGL 任务归属契约（真机冻死事故的静态防线）。

背景（2026-10-06 真机返工记录）：设备进入「其他」的语音输入页、说完话之后**整机冻死**，
串口现场是

    E task_wdt: Task watchdog got triggered. The following tasks/users did not reset
                the watchdog in time: - IDLE (CPU 0)
    E task_wdt: CPU 0: app_task（当时表现为 app_voice/app_task 长跑不喂狗）
    MEPC 落在 lv_inv_area（LVGL 刷新区失效逻辑）里死循环

根因不是内存，也**不是** LVGL 有 bug：识别结果通过链路消息队列送到 **app_task**，
而回调 `app_question_on_voice_result()` 直接调用了 `show_panel()` /
`refresh_question_rows()` / `refresh_question_hint()` —— 这些是 LVGL 调用。
LVGL 只允许在 LVGL 任务里使用（本仓库的按键路径都要先 `bsp_lvgl_lock(200)`），
两个任务并发碰它 → 内部链表被破坏 → 死循环 → 整机冻死。

这个错误在宿主机上**编译通过、跑测试也通过**，只有真机才会冻死。所以把它变成静态契约：
跑在非 LVGL 任务里的入口，函数体里不许出现 LVGL 调用，也不许直接调用会碰界面的内部函数。
"""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
SCREENS = "main/app_screens.c"

# 跑在 app_task / 链路回调里的入口（见 app.c 的 process_message 与 post_* 调用点）。
NON_LVGL_ENTRYPOINTS = [
    "app_question_on_voice_result",
    "app_screens_post_approval",
    "app_screens_post_approval_done",
    "app_screens_post_balance",
    "app_screens_post_voice_result",
    "app_screens_post_question",
    "app_screens_post_question_done",
    "app_screens_request_balance",
]

# 这些内部函数会碰 LVGL：非 LVGL 任务里的入口连它们也不能调。
LVGL_TOUCHING_HELPERS = [
    "show_panel",
    "refresh_question_rows",
    "refresh_question_hint",
    "refresh_approval_options",
    "app_screens_show_approval",
    "app_screens_show_voice_result",
    "app_screens_show_balance",
    "app_screens_hide",
    "ui_theme_text",
    "make_overlay",
    "clean(",  # 无害，但保持列表短：只列真正碰界面的
]

LVGL_CALL = re.compile(r"\blv_[a-z0-9_]+\s*\(")


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


def function_body(source: str, name: str) -> str:
    """取出函数体（含嵌套大括号），取不到就报错 —— 宁可失败也不要静默跳过。

    ★ 先剥掉注释再找定义：函数名经常出现在注释里（例如
    `// 直到下一个 toast 或 app_ui_hide_toast() 显式隐藏。`），
    直接在带注释的源码上匹配会把**下一个函数**的函数体当成它的（真踩过这个假阳性）。
    """
    source = strip_comments(source)
    match = re.search(rf"\b{re.escape(name)}\s*\([^;]*?\)\s*\{{", source)
    if not match:
        raise AssertionError(f"function not found: {name}")
    start = match.end() - 1
    depth = 0
    for index in range(start, len(source)):
        char = source[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise AssertionError(f"function body is unterminated: {name}")


def strip_comments(body: str) -> str:
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    return re.sub(r"//[^\n]*", "", body)


class LvglTaskContract(unittest.TestCase):
    def test_non_lvgl_entrypoints_do_not_touch_lvgl(self) -> None:
        source = read(SCREENS)
        for name in NON_LVGL_ENTRYPOINTS:
            body = strip_comments(function_body(source, name))
            lvgl_calls = LVGL_CALL.findall(body)
            self.assertEqual(
                lvgl_calls, [],
                f"{name}() 跑在非 LVGL 任务里，却直接调用了 LVGL：{lvgl_calls}。"
                "只允许写结构 + 置脏标志，界面动作交给 app_screens_tick_*/consume_* 在 LVGL 任务里做。",
            )
            for helper in LVGL_TOUCHING_HELPERS:
                if helper == "clean(":
                    continue
                self.assertNotIn(
                    f"{helper}(", body,
                    f"{name}() 调用了会碰 LVGL 的 {helper}()；非 LVGL 任务里只允许写结构 + 置脏标志。",
                )

    def test_voice_result_is_delivered_through_a_dirty_flag(self) -> None:
        source = read(SCREENS)
        body = strip_comments(function_body(source, "app_question_on_voice_result"))
        self.assertIn(
            "s_q_voice_result_dirty = true", body,
            "识别结果必须只置脏标志，由 LVGL 任务消费（否则会并发访问 LVGL）。",
        )
        tick = strip_comments(function_body(source, "app_screens_tick_question"))
        self.assertIn(
            "consume_q_voice_result()", tick,
            "app_screens_tick_question()（LVGL 任务）必须消费语音识别结果。",
        )

    def test_post_family_never_touches_lvgl(self) -> None:
        """`post_/set_/hide_/request_` 家族是给非 LVGL 任务用的投递接口，体内不许有 LVGL 调用。

        真机事故 2（2026-10-06）：`app_ui_hide_toast()` 里直接
        `lv_obj_add_flag` + `lv_timer_delete`，而它被**语音任务**在录音收尾时调用 ——
        与 LVGL 任务并发删定时器/改标志，内部链表损坏后 lv_inv_area 死循环、整机冻死。
        现象是"按住说话说完一松手就死机"。
        """
        family = re.compile(r"^(app_ui_(post|set|hide|request)_[a-z_]+|app_screens_post_[a-z_]+|app_question_on_voice_result)$")
        offenders: list[str] = []
        for path in ("main/app_ui.c", "main/app_screens.c"):
            source = read(path)
            for name in set(re.findall(r"^[a-z_][a-z0-9_ *]*\b(app_(?:ui|screens)_[a-z_]+|app_question_on_voice_result)\s*\(", source, re.M)):
                if not family.match(name):
                    continue
                body = strip_comments(function_body(source, name))
                calls = LVGL_CALL.findall(body)
                if calls:
                    offenders.append(f"{path}:{name} -> {calls}")
        self.assertEqual(
            offenders, [],
            "这些接口跑在非 LVGL 任务里，只能写结构 + 置脏标志，实际却调用了 LVGL：\n" + "\n".join(offenders),
        )
        # 防空转：正则匹配不到函数时这条断言会"永远通过"，那才是真正的危险。
        family = re.compile(r"^(app_ui_(post|set|hide|request)_[a-z_]+|app_screens_post_[a-z_]+|app_question_on_voice_result)$")
        checked = [
            name
            for path in ("main/app_ui.c", "main/app_screens.c")
            for name in set(re.findall(r"^[a-z_][a-z0-9_ *]*\b(app_(?:ui|screens)_[a-z_]+|app_question_on_voice_result)\s*\(", strip_comments(read(path)), re.M))
            if family.match(name)
        ]
        self.assertGreaterEqual(
            len(checked), 10,
            f"只匹配到 {len(checked)} 个投递接口（预期 ≥10）—— 正则或命名变了，这条契约正在空转。",
        )

    def test_voice_task_ui_calls_are_lvgl_free(self) -> None:
        """语音任务（app_voice.c）调用的每个 UI 接口都必须在"投递家族"里且不碰 LVGL。"""
        family = re.compile(r"^(app_ui_(post|set|hide|request)_[a-z_]+|app_screens_post_[a-z_]+|app_question_on_voice_result)$")
        voice = read("main/app_voice.c")
        called = set(re.findall(r"\b(app_(?:ui|screens)_[a-z_]+|app_question_on_voice_result)\s*\(", voice))
        self.assertTrue(called, "app_voice.c 应当调用 UI 投递接口（否则录制状态无法反馈给界面）")
        for name in sorted(called):
            self.assertTrue(
                family.match(name),
                f"语音任务调用了 {name}() —— 它不在投递家族里，无法保证不碰 LVGL；"
                "要么改用 app_ui_post_*/set_*/hide_*，要么补上 bsp_lvgl_lock。",
            )

    def test_key_path_holds_the_lvgl_lock(self) -> None:
        """按键路径跑在 app_task，必须先取 LVGL 锁才能碰界面。"""
        app = read("main/app.c")
        body = strip_comments(function_body(app, "handle_key"))
        lock_at = body.find("bsp_lvgl_lock")
        ui_at = body.find("app_ui_handle_key")
        self.assertNotEqual(lock_at, -1, "handle_key() 必须取 bsp_lvgl_lock")
        self.assertNotEqual(ui_at, -1, "handle_key() 应当调用 app_ui_handle_key")
        self.assertLess(lock_at, ui_at, "必须先取锁，再调用界面层（app_ui_handle_key）")


if __name__ == "__main__":
    unittest.main(verbosity=2)
