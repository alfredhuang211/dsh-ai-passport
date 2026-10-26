// main/app_ui_theme.h —— 界面设计约束（"bit 极简"风格的单一事实来源）。
//
// 设计意图：像一台 80 年代终端/掌机。硬边、无圆角、无渐变、无阴影；
// 只用 1bpp 点阵字体（Press Start 2P）与少数几个纯色块。
//
// ★ 一条硬约束：**界面文本只用 ASCII**。
//   原因不是审美偏好，而是踩过的坑：LVGL 内置的 CJK 子集不覆盖本应用文案
//   （48 个中文字里缺 23 个 → 满屏方框），自带字体子集也仍然显示成方框，
//   两次都没能在真机上拿到可读的中文。ASCII 是点阵字体必然覆盖的，
//   而方框/空白是**无法在设备上自测**的那类故障（没有截图能力时只能靠用户拍照）。
//   因此这一版改为 ASCII + 图标化的状态指示（用色块画，不依赖任何字形）。
//   以后若要恢复中文，正确顺序是：先搞定"能在设备上看见屏幕"的手段，
//   再引入 CJK 字体，否则又是在盲改。
#pragma once

#include "lvgl.h"

#include <stdint.h>

// ── 调色板：终端绿 + 冷灰，少量警告色 ─────────────────────────────────────
#define UI_BG 0x0B0F14        // 近黑（不是纯黑，留一点层次）
#define UI_PANEL 0x111823     // 面板底
#define UI_LINE 0x243447      // 分隔线 / 边框
#define UI_FG 0xC8D6E5        // 主文本（冷白）
#define UI_DIM 0x5A6B7D       // 次要文本
#define UI_ACCENT 0x38E07B    // 终端绿（正常/在线）
#define UI_WARN 0xFFB020      // 琥珀（进行中/注意）
#define UI_ERR 0xFF4D4D       // 红（错误/危险）
#define UI_INFO 0x4DB8FF      // 青蓝（信息/选中）

// 列表行与层次（新交互设计 v2）
#define UI_ROW_SEL 0x1E2C3E   // 选中行底色：明显比背景亮，小屏上才看得出
#define UI_FG_DIM 0x8FA3B8    // 次级前景：未选中行的标题

// ── 字体 ──────────────────────────────────────────────────────────────────
//
// 界面是**中文**，字体分两层，由 LVGL 的 fallback 机制串起来：
//
//   app_ui_font_cjk   中文子集（思源黑体, 2bpp, 20px）← 主力，line_height 24
//   app_ui_font_8     点阵 ASCII（Press Start 2P, 1bpp, 8px）
//   app_ui_font_16    点阵 ASCII（Press Start 2P, 1bpp, 16px）← 也带 .fallback → 中文
//
// 这样 ASCII 保持"极客 bit"的点阵观感，中文由思源黑体渲染，
// 而**控件上只需指定一个字体**，不用在业务代码里判断字符属于哪种。
//
// ★ 中文字形踩过两次坑（内置 CJK 子集缺 23 字；自制子集仍显示方框），
//   因此字形集由 tools/make-fonts.py 从源码字面量**自动提取**，
//   并且脚本会解析生成产物里的真实字形表来核对覆盖率，
//   缺字直接报错。校验已接入 validate.sh --static。
#define UI_FONT_S (&app_ui_font_8)
#define UI_FONT_M (&app_ui_font_16)
// 中文标题用 16px 点阵位图的 fallback（思源黑体）。
// 单独暴露一个名字，便于"只想要中文观感"的地方显式使用。
#define UI_FONT_CJK (&app_ui_font_cjk)   // 20px, 2bpp（正文/标题/选项）
// 16px 中文：**提示行专用**。20px 一行约 10 个汉字，底栏/按键说明经常塞不下；
// 16px 每行约 13 个汉字，行高 20px，字库约 +30KB（见 tools/make-fonts.py）。
#define UI_FONT_CJK16 (&app_ui_font_cjk16)   // 16px, 2bpp
// 超大字号：用于"状态大字"。设备可能被放在桌角，状态必须隔一两米也能认出，
// 因此单独生成 40px 档（见 tools/make-fonts.py 的 UI_FONT_XL 说明）。
#define UI_FONT_XL (&app_ui_font_xl)

extern const lv_font_t app_ui_font_8;
extern const lv_font_t app_ui_font_16;
extern const lv_font_t app_ui_font_cjk;
extern const lv_font_t app_ui_font_cjk16;
extern const lv_font_t app_ui_font_xl;

// ── 布局栅格（面板可视区，全部硬边）──────────────────────────────────────
//
// ★ 面板不是整块 240×320：外壳有圆角，边缘约 10px 的内容会被遮挡
//   （用户反馈"屏幕有一点遮挡"）。因此**整个界面按安全区布局**，
//   四边各让出 UI_INSET，所有坐标从安全区算起，而不是从物理边缘算起。
//   这样做而不是整体缩放：字体会被缩放糊掉，而重新排版只是挪坐标。
#define UI_INSET 10
#define UI_VISUAL_W (240)               // 物理可视宽度（背景铺满它）
#define UI_VISUAL_H (320)               // 物理可视高度

#define UI_W (UI_VISUAL_W - UI_INSET * 2)   // 安全区宽度 = 220
#define UI_H (UI_VISUAL_H - UI_INSET * 2)   // 安全区高度 = 300

// 安全区内的相对坐标；渲染时统一加 UI_INSET 偏移（见 app_ui.c 的 POS 宏）
#define UI_PAD 6

// ── 字体度量（来自 main/fonts/ 的生成产物；改字体后必须同步核对）──────────
//
// ★ 版面的每个纵向尺寸都从这里推出，不写死像素偏移。踩过的坑：
//   给底栏第二行写死 y=19，而中文行高 24px > 19px，两行文字直接压在一起。
// ★ 行间留白同样要算：两个 24px 行框若首尾相接（间隙 0），20px 的中文
//   视觉上仍是一坨。行框之间至少留 4px，宁可少显示一行也不挤。
#define UI_LINE_H_CJK 24        // app_ui_font_cjk.line_height（20px 字号，字形高 22）
#define UI_GLYPH_H_CJK 22
#define UI_LINE_H_XL 38         // app_ui_font_xl.line_height
#define UI_TEXT_LINE_SPACE 4    // ui_theme_text 默认行间距
#define UI_TEXT_PITCH_CJK (UI_LINE_H_CJK + UI_TEXT_LINE_SPACE)   // 多行行距 = 28

// ── 版面规格（安全区坐标，由 tools/verify-layout.py 验证）────────────────
//
// 精简版的纵向预算（安全区 220×300）：
//     状态条   0 .. 28    高 28   链路色块 + 应用名（24px 行框居中）
//     分隔线        28
//     状态主体 29 .. 240  高 211  状态大字 + 补充说明（占满，让状态成为视觉主体）
//     分隔线        241
//     底栏   242 .. 298   高 56   两行按键提示：行框 24 + 行间隙 4，上下各留 2
//     下边距 298 .. 300   高 2
#define UI_STATUSBAR_Y 0
#define UI_STATUSBAR_H 28
#define UI_FOOTER_H 56
#define UI_FOOTER_Y (UI_H - UI_FOOTER_H - 2)    // = 242
#define UI_BODY_Y (UI_STATUSBAR_H + 1)          // = 29
#define UI_BODY_H (UI_FOOTER_Y - UI_BODY_Y - 2) // = 211

// 状态主体内部（相对色块左上角）
//     状态大字 36 .. 74    XL 行框 38，"偏上放置"给下方说明留呼吸
//     补充说明 84 .. 164   最多 3 行（超出省略号），绝不允许长标题压到底栏
#define UI_STATE_TEXT_Y 36
#define UI_STATE_HINT_Y (UI_STATE_TEXT_Y + UI_LINE_H_XL + 10)   // = 84
#define UI_STATE_HINT_LINES 3
#define UI_STATE_HINT_H (UI_STATE_HINT_LINES * UI_TEXT_PITCH_CJK - UI_TEXT_LINE_SPACE) // = 80

// 底栏两行提示的行框位置（只显示一行时用 UI_CENTER_Y 整体居中）
#define UI_FOOTER_LINE1_Y 2
#define UI_FOOTER_LINE2_Y (UI_FOOTER_LINE1_Y + UI_LINE_H_CJK + 4)   // = 30

// 提示条（浮在状态主体上的短暂通知）
#define UI_TOAST_H 68
#define UI_TOAST_TEXT_Y 8
#define UI_TOAST_TEXT_LINES 2
#define UI_TOAST_TEXT_H (UI_TOAST_TEXT_LINES * UI_TEXT_PITCH_CJK - UI_TEXT_LINE_SPACE) // = 52

// ── 覆盖层规格（审批 / 余额，全屏覆盖安全区）──────────────────────────────
#define OV_TITLE_H 28
// ── 系统设置页（覆盖层）──
//     标题   0 .. 28
//     行 1   44 .. 74    高 30（行间 4，与追问页行同款样式）
//     行 2   78 .. 108
//     行 3  112 .. 142
//     行 4  146 .. 176
//     配对行 180 .. 232  高 52 —— 两行：状态行 + 主机名行。
//     ★ 配对行必须两行：单行只有约 9 个汉字宽，"已配对（主机名）"会被
//       省略号截断（真机反馈"已配对没显示全"）。主机名单独一行放得下
//       常见主机名，超长才落省略号。
//     底栏  276 .. 300
#define OV_SET_ROWS_Y (OV_TITLE_H + 16)   // = 44
#define OV_SET_PAIR_H (2 * UI_LINE_H_CJK + UI_TEXT_LINE_SPACE)   // = 52（两行中文）
#define OV_FOOT_H 24

// 审批页（覆盖层内坐标，2026-10-06 二改：**去掉倒计时**，只有一行摘要 + 按钮）：
//     标题     0 .. 28
//     摘要    38 .. 62    单行，超长省略号（完整信息在电脑卡片上；高风险=红、普通=黄）
//     选项 1   98 .. 128   高 30（行间 4）
//     选项 2  132 .. 162   高 30
//     底栏    276 .. 300   高 24
//   审批页停留到用户在电脑端或设备端作答为止，不再有倒计时条与倒计时文字。
#define OV_APPR_TOOL_Y (OV_TITLE_H + 10)                          // = 38（摘要行）
#define OV_APPR_OPT_H 30
#define OV_APPR_OPT_GAP 4
#define OV_APPR_OPTS_Y (OV_APPR_TOOL_Y + UI_LINE_H_CJK + 36)      // = 98

// 余额页（覆盖层内坐标）：
//     标题     0 .. 28
//     总额    40 .. 64
//     明细    80 .. 200   最多 4 行，行距 8（多行数字明细舒展不挤）
#define OV_BAL_TOTAL_Y (OV_TITLE_H + 12)                          // = 40
#define OV_BAL_LINE_SPACE 8
#define OV_BAL_LINES_Y (OV_BAL_TOTAL_Y + UI_LINE_H_CJK + 16)      // = 80
#define OV_BAL_LINES 4
#define OV_BAL_LINES_H (OV_BAL_LINES * (UI_LINE_H_CJK + OV_BAL_LINE_SPACE) - OV_BAL_LINE_SPACE) // = 120

// 识别结果卡（覆盖层内坐标，docs/06 §2）：
//     标题     0 .. 28
//     文本    38 .. 268   内部滚动（识别文本长短不定，固定行数会截断长句）
//     底栏   276 .. 300
#define OV_VOICE_TEXT_Y (OV_TITLE_H + 10)                                    // = 38
// 结果卡在正文与底栏之间留**三行**按键说明（用户指定）：
//     单击上键 删除/返回 / 单击确认 填入 / 长按确认 发送
// 提示行用 16px 中文（行高 20px）。
// ★ 高度必须按**行距**算，不能只按行高：ui_theme_text 默认带 UI_TEXT_LINE_SPACE(4) 行距，
//   三行需要 3*20 + 2*4 = 68px。早先用 60px 时第三行「长按确认 发送」被裁掉（真机反馈）。
//   写法与主题里其它多行控件一致：行数 × 行距 − 行距。
#define UI_LINE_H_CJK16 20
#define UI_TEXT_PITCH_CJK16 (UI_LINE_H_CJK16 + UI_TEXT_LINE_SPACE)         // = 24
#define OV_VOICE_KEYS_LINES 3
#define OV_VOICE_KEYS_H (OV_VOICE_KEYS_LINES * UI_TEXT_PITCH_CJK16 - UI_TEXT_LINE_SPACE)  // = 68
// 结果卡不收底栏（共用底栏被隐藏），三行说明直接**贴底排**，省掉原来那一节空白。
#define OV_VOICE_KEYS_Y (UI_H - OV_VOICE_KEYS_H - 6)                       // = 234
#define OV_VOICE_TEXT_H (OV_VOICE_KEYS_Y - OV_VOICE_TEXT_Y - 6)            // = 190

// 追问 / 计划评审页（2026-10-06 改版：一行极短摘要 + 按钮行，正文在电脑卡片）：
//     标题     0 .. 28     「问题 i/N」或「计划评审」
//     摘要    34 .. 58     单行，超长省略号
//     行列表  62 .. 268    行高 30、行距 4；可见 6 行，多行滚动
//     底栏   276 .. 300
#define OV_Q_TEXT_Y (OV_TITLE_H + 6)                        // = 34（摘要行）
#define OV_Q_TEXT_H UI_LINE_H_CJK                           // = 24
#define OV_Q_LIST_Y (OV_Q_TEXT_Y + OV_Q_TEXT_H + 4)         // = 62
#define OV_Q_LIST_H (UI_H - OV_FOOT_H - OV_Q_LIST_Y - 8)    // = 206

// 语音输入页（自定义答案）：标题 + 居中状态/识别文字 + 底栏
#define OV_QV_TEXT_Y 96
#define OV_QV_TEXT_H 48
#define OV_Q_ROW_H 30
#define OV_Q_ROW_GAP 4

// 文本在容器内垂直居中（避免写死偏移）
#define UI_CENTER_Y(container_h, line_h) (((container_h) - (line_h)) / 2)

// ── 安全区 → 屏幕坐标 ─────────────────────────────────────────────────────
// 所有界面元素按安全区内的相对坐标排版，再统一加 INSET 偏移。
// 这样调整圆角遮挡只需改 UI_INSET 一个数字，不必重算每个坐标。
#define PX(x) ((x) + UI_INSET)
#define PY(y) ((y) + UI_INSET)

// ── 风格助手 ──────────────────────────────────────────────────────────────

// 硬边矩形（无圆角、无阴影），风格的基础图元。
static inline void ui_theme_rect(lv_obj_t *obj, uint32_t bg)
{
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg), 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

// 带 1px 边框的面板。
static inline void ui_theme_box(lv_obj_t *obj, uint32_t bg, uint32_t border)
{
    ui_theme_rect(obj, bg);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(border), 0);
}

// 文本标签（点阵/中文字体）。
static inline lv_obj_t *ui_theme_text(lv_obj_t *parent, const char *text,
                                      const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_letter_space(label, 0, 0);
    // 默认给 4px 行间距缓冲，彻底规避回退字体行高不一致导致的行间挤压
    lv_obj_set_style_text_line_space(label, 4, 0);
    return label;
}
