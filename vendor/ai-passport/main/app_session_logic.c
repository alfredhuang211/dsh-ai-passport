// main/app_session_logic.c —— 多会话列表/详情的纯逻辑实现（协议 v2）。
//
// 这里刻意不 include LVGL / NimBLE / FreeRTOS：host 测试与真机跑的是同一份代码。
// 数据来源见 docs/09 §3：主机把数组拍平成 `|` 分隔的标量，设备端用 ap_flat_next
// 逐字段解码（`¦`→`|`、`╲`→`\`）。**不能**假设各字段串的 field count 一致：
// 主机允许空标题，数组末尾为空时 join 结果以 '|' 结尾，而 ap_flat_next 迭代到
// 结尾空字段会返回 false —— 因此以 handles 的字段数 n 为准，逐项容错补齐。
#include "app_session_logic.h"

#include "app_proto_logic.h"

#include <string.h>

static const char *const STATE_NAMES[] = {
    [AP_SESSION_STATE_UNKNOWN] = "unknown",
    [AP_SESSION_STATE_RUNNING] = "running",
    [AP_SESSION_STATE_WAITING] = "waiting",
    [AP_SESSION_STATE_ERROR] = "error",
    [AP_SESSION_STATE_ABORTED] = "aborted",
    [AP_SESSION_STATE_IDLE] = "idle",
};

bool ap_session_state_parse(const char *name, uint8_t *out)
{
    if (!name || !name[0]) return false;
    for (int i = AP_SESSION_STATE_RUNNING; i <= AP_SESSION_STATE_IDLE; i++) {
        if (strcmp(name, STATE_NAMES[i]) == 0) {
            if (out) *out = (uint8_t)i;
            return true;
        }
    }
    return false;
}

const char *ap_session_state_name(uint8_t state)
{
    if (state > AP_SESSION_STATE_IDLE) return STATE_NAMES[AP_SESSION_STATE_UNKNOWN];
    return STATE_NAMES[state];
}

/** UTF-8 安全复制：超长时在完整字符边界截断（不产出半个汉字）。 */
static void copy_utf8(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t max = dst_size - 1;
    size_t n = strlen(src);
    if (n > max) {
        n = max;
        // 若截断点落在续字节上，说明切在字符中间 —— 回退到该字符的首字节之前。
        while (n > 0 && (((unsigned char)src[n]) & 0xC0) == 0x80) n--;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/** 取下一个扁平字段；src 耗尽时输出空串（不修改 pos，供尾部空字段补齐）。 */
static void take_field(const char *src, size_t *pos, char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!src || !pos) return;
    (void)ap_flat_next(src, pos, out, out_size);
}

void ap_session_list_clear(ap_session_list_t *list)
{
    if (!list) return;
    memset(list, 0, sizeof(*list));
}

bool ap_session_list_apply(ap_session_list_t *list,
                           const char *handles, const char *titles,
                           const char *states, const char *unread, const char *sel)
{
    if (!list) return false;
    const size_t n = ap_flat_count(handles);
    if (n > AP_SESSION_MAX) {
        // 超上限说明主机/固件版本不匹配：宁可清空并报错，也不要半旧半新。
        ap_session_list_clear(list);
        return false;
    }

    ap_session_list_clear(list);
    size_t pos_h = 0;
    size_t pos_t = 0;
    size_t pos_s = 0;
    size_t pos_u = 0;
    char field_h[8];
    char field_t[AP_SESSION_TITLE_BYTES * 2 + 8];
    char field_s[16];
    char field_u[8];

    for (size_t i = 0; i < n; i++) {
        take_field(handles, &pos_h, field_h, sizeof(field_h));
        take_field(titles, &pos_t, field_t, sizeof(field_t));
        take_field(states, &pos_s, field_s, sizeof(field_s));
        take_field(unread, &pos_u, field_u, sizeof(field_u));

        // handle 是设备侧唯一稳定标识；为空/重复的行直接跳过（主机不变量被破坏时自保）。
        if (!field_h[0]) continue;
        if (ap_session_find(list, field_h) >= 0) continue;

        ap_session_row_t *row = &list->rows[list->count];
        // 只保留前 2 个字符（协议规定 2 位十六进制），多余字符丢弃。
        row->handle[0] = field_h[0];
        row->handle[1] = field_h[1] ? field_h[1] : '\0';
        row->handle[2] = '\0';
        copy_utf8(row->title, sizeof(row->title), field_t);

        uint8_t state = AP_SESSION_STATE_IDLE;
        if (field_s[0]) {
            if (!ap_session_state_parse(field_s, &state)) state = AP_SESSION_STATE_UNKNOWN;
        }
        row->state = state;
        row->unread = (field_u[0] == '1');
        row->valid = true;
        list->count++;
    }

    if (sel && sel[0]) {
        // 选中的 handle 不在列表里（会话已关闭/重排）时保持"未选中"。
        (void)ap_session_set_selected(list, sel);
    }
    return true;
}

int ap_session_find(const ap_session_list_t *list, const char *handle)
{
    if (!list || !handle || !handle[0]) return -1;
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->rows[i].handle, handle) == 0) return i;
    }
    return -1;
}

const ap_session_row_t *ap_session_row(const ap_session_list_t *list, int index)
{
    if (!list || index < 0 || index >= list->count) return NULL;
    return &list->rows[index];
}

const ap_session_row_t *ap_session_selected_row(const ap_session_list_t *list)
{
    if (!list || !list->selected[0]) return NULL;
    const int index = ap_session_find(list, list->selected);
    if (index < 0) return NULL;
    return &list->rows[index];
}

bool ap_session_set_selected(ap_session_list_t *list, const char *handle)
{
    if (!list) return false;
    const int index = ap_session_find(list, handle);
    if (index < 0) {
        list->selected[0] = '\0';
        return false;
    }
    copy_utf8(list->selected, sizeof(list->selected), list->rows[index].handle);
    return true;
}

void ap_session_clear_selected(ap_session_list_t *list)
{
    if (list) list->selected[0] = '\0';
}

void ap_session_apply_done(ap_session_list_t *list, const char *handle, const char *result)
{
    const int index = ap_session_find(list, handle);
    if (index < 0) return;
    ap_session_row_t *row = &list->rows[index];

    if (result && strcmp(result, "error") == 0) {
        row->state = AP_SESSION_STATE_ERROR;
    } else if (result && strcmp(result, "aborted") == 0) {
        row->state = AP_SESSION_STATE_ABORTED;
    } else {
        // completed（含未知 result）：回到空闲，交给主机下一次列表推送纠偏。
        row->state = AP_SESSION_STATE_IDLE;
    }

    // 未选中 = 用户没在设备上看这条会话 → 记未读，列表行显示小圆点。
    row->unread = (strcmp(list->selected, row->handle) != 0);
}

void ap_session_detail_clear(ap_session_detail_t *detail)
{
    if (!detail) return;
    memset(detail, 0, sizeof(*detail));
    detail->step = -1;
}

void ap_session_detail_apply(ap_session_detail_t *detail,
                             const char *handle, const char *title, const char *state,
                             int step, int step_total, const char *tool,
                             uint32_t elapsed_ms, const char *summary)
{
    if (!detail) return;
    ap_session_detail_clear(detail);
    if (!handle || !handle[0]) return;

    detail->valid = true;
    detail->handle[0] = handle[0];
    detail->handle[1] = handle[1] ? handle[1] : '\0';
    detail->handle[2] = '\0';
    copy_utf8(detail->title, sizeof(detail->title), title);
    copy_utf8(detail->tool, sizeof(detail->tool), tool);
    copy_utf8(detail->summary, sizeof(detail->summary), summary);

    uint8_t st = AP_SESSION_STATE_UNKNOWN;
    if (state && state[0] && ap_session_state_parse(state, &st)) {
        detail->state = st;
    } else {
        detail->state = AP_SESSION_STATE_UNKNOWN;
    }
    detail->step = step;
    detail->step_total = step_total > 0 ? step_total : 0;
    detail->elapsed_ms = elapsed_ms;
}
