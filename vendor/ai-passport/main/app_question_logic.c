// 追问 / 计划评审的纯逻辑实现。接口语义见 app_question_logic.h。
#include "app_question_logic.h"

#include <stdio.h>
#include <string.h>

int ap_q_split_options(const char *csv, char labels[][AP_Q_LABEL_SIZE], int max)
{
    int count = 0;
    if (!csv || !labels || max <= 0) return 0;

    const char *p = csv;
    while (*p && count < max) {
        char *slot = labels[count];
        size_t n = 0;
        // 每个标签收满 AP_Q_LABEL_SIZE-1 字节或遇到分隔符/串尾为止
        while (*p && *p != '|' && n + 1 < AP_Q_LABEL_SIZE) slot[n++] = *p++;
        slot[n] = '\0';
        if (*p == '|') p++;          // 跳过本段的分隔符
        if (n > 0) count++;          // 空段（"a||b"）不占选项位
    }
    return count;
}

int ap_q_cursor_move(int cursor, int rows, int delta)
{
    if (rows <= 0) return 0;
    int next = (cursor + delta) % rows;
    if (next < 0) next += rows;
    return next;
}

void ap_q_select_single(bool *selected, int count, int index)
{
    if (!selected || count <= 0) return;
    for (int i = 0; i < count; i++) selected[i] = (i == index);
}

void ap_q_select_toggle(bool *selected, int count, int index)
{
    if (!selected || count <= 0 || index < 0 || index >= count) return;
    selected[index] = !selected[index];
}

size_t ap_json_escape(char *out, size_t out_size, const char *src)
{
    if (!out || out_size == 0) return 0;
    size_t n = 0;
    // 永远给结尾 0 留位；不够就停在完整转义序列之前
    for (const unsigned char *p = (const unsigned char *)(src ? src : ""); *p; p++) {
        char seq[8];
        size_t seq_len = 0;
        if (*p == '"' || *p == '\\') {
            seq[0] = '\\'; seq[1] = (char)*p; seq[2] = '\0'; seq_len = 2;
        } else if (*p < 0x20) {
            // 控制字符统一走 \u00XX（6 字节），避免产出裸换行打断 JSON
            snprintf(seq, sizeof(seq), "\\u%04x", (unsigned)*p);
            seq_len = 6;
        } else {
            seq[0] = (char)*p; seq[1] = '\0'; seq_len = 1;
        }
        if (n + seq_len + 1 > out_size) break;
        memcpy(out + n, seq, seq_len);
        n += seq_len;
    }
    out[n] = '\0';
    return n;
}

// 组装辅助：可选前导逗号 + 转义后的 "key":"value" 片段追加
static int append_kv_str(char *out, size_t out_size, int off, bool comma, const char *key, const char *value)
{
    char esc[512];
    ap_json_escape(esc, sizeof(esc), value ? value : "");
    return snprintf(out + off, (size_t)(out_size > (size_t)off ? out_size - (size_t)off : 0),
                    "%s\"%s\":\"%s\"", comma ? "," : "", key, esc);
}

int ap_q_build_pick_json(char *out, size_t out_size, const char *call_id, const char *qid,
                         const char labels[][AP_Q_LABEL_SIZE], const bool *selected, int count,
                         const char *custom, bool skipped)
{
    if (!out || out_size == 0) return -1;
    int off = snprintf(out, out_size, "{\"type\":\"question.pick\"");
    if (off < 0) return off;
    off += append_kv_str(out, out_size, off, true, "callId", call_id);
    off += append_kv_str(out, out_size, off, true, "qid", qid);
    off += snprintf(out + off, (size_t)(out_size > (size_t)off ? out_size - (size_t)off : 0),
                    ",\"selected\":[");
    bool first = true;
    for (int i = 0; i < count; i++) {
        if (!selected || !selected[i]) continue;
        char esc[AP_Q_LABEL_SIZE * 2];
        ap_json_escape(esc, sizeof(esc), labels ? labels[i] : "");
        // ★ 逗号跟"是否已输出过元素"走，不能看 selected[i-1]：
        //   多选跳着勾（勾 0 和 2）时 selected[1]=false，按上一个判断会漏掉逗号，
        //   产出非法 JSON —— 现象是"跳着多选就发不出去"。
        off += snprintf(out + off, (size_t)(out_size > (size_t)off ? out_size - (size_t)off : 0),
                        "%s\"%s\"", first ? "" : ",", esc);
        first = false;
    }
    off += snprintf(out + off, (size_t)(out_size > (size_t)off ? out_size - (size_t)off : 0), "]");
    off += append_kv_str(out, out_size, off, true, "custom", custom);
    off += snprintf(out + off, (size_t)(out_size > (size_t)off ? out_size - (size_t)off : 0),
                    ",\"skipped\":%s}", skipped ? "1" : "0");
    return off;
}

int ap_q_build_answer_json(char *out, size_t out_size, const char *call_id, const char *action)
{
    if (!out || out_size == 0) return -1;
    int off = snprintf(out, out_size, "{\"type\":\"question.answer\"");
    if (off < 0) return off;
    off += append_kv_str(out, out_size, off, true, "callId", call_id);
    off += append_kv_str(out, out_size, off, true, "action", action ? action : "submit");
    off += snprintf(out + off, (size_t)(out_size > (size_t)off ? out_size - (size_t)off : 0), "}");
    return off;
}

int ap_q_build_nav_json(char *out, size_t out_size, const char *call_id, int index)
{
    if (!out || out_size == 0) return -1;
    int off = snprintf(out, out_size, "{\"type\":\"question.nav\"");
    if (off < 0) return off;
    off += append_kv_str(out, out_size, off, true, "callId", call_id);
    off += snprintf(out + off, (size_t)(out_size > (size_t)off ? out_size - (size_t)off : 0),
                    ",\"index\":%d}", index);
    return off;
}

// ── 行模型 ────────────────────────────────────────────────────────────────
//
// 行号约定（与 docs/06 §3 的行模型一致）：
//   计划评审：0 要求修改 / 1 同意执行（固定 2 行）
//   选择题：  0..option_count-1 选项 / option_count 其他 /
//             option_count+1 [下一题（非末题才有）] / 最后一行 跳过此题

int ap_q_row_count(bool is_plan, int option_count, int index, int total)
{
    if (is_plan) return 2;
    if (option_count < 0) option_count = 0;
    return option_count + 2 + (ap_q_row_has_next(false, index, total) ? 1 : 0);
}

int ap_q_row_custom(bool is_plan, int option_count)
{
    if (is_plan) return -1;   // 计划页没有 custom 行
    return option_count < 0 ? 0 : option_count;
}

bool ap_q_row_has_next(bool is_plan, int index, int total)
{
    return !is_plan && (index + 1 < total);
}

int ap_q_row_next(bool is_plan, int option_count, int index, int total)
{
    if (!ap_q_row_has_next(is_plan, index, total)) return -1;
    return option_count < 0 ? 1 : option_count + 1;
}

int ap_q_row_skip(bool is_plan, int option_count, int index, int total)
{
    return ap_q_row_count(is_plan, option_count, index, total) - 1;
}

int ap_q_row_text(char *out, size_t out_size,
                  bool is_plan, bool multi_select,
                  const bool *selected, const char labels[][AP_Q_LABEL_SIZE],
                  int option_count, int index, int total,
                  const char *custom, int row)
{
    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    if (row < 0 || row >= ap_q_row_count(is_plan, option_count, index, total)) return 0;

    if (is_plan) {
        return snprintf(out, out_size, "%s", row == 0 ? "要求修改" : "同意执行");
    }
    if (row < option_count) {
        const bool sel = selected && selected[row];
        // labels[row] 是数组（恒非空指针），只判 labels 本身
        return snprintf(out, out_size, "%s %s",
                        sel ? (multi_select ? "[x]" : "(*)")
                            : (multi_select ? "[ ]" : "( )"),
                        labels ? labels[row] : "");
    }
    if (row == ap_q_row_custom(is_plan, option_count)) {
        return snprintf(out, out_size, "其他：%s",
                        (custom && custom[0]) ? custom : "语音输入");
    }
    if (row == ap_q_row_next(is_plan, option_count, index, total)) {
        return snprintf(out, out_size, "下一题");
    }
    return snprintf(out, out_size, "跳过此题");
}

