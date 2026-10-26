// main/app_session_logic.h —— 多会话列表/详情的纯逻辑（协议 v2，docs/09 §6）。
//
// 与 app_question_logic.h 同一个理由拆出来：只放**纯逻辑**，不碰 LVGL / NimBLE /
// FreeRTOS，host 侧可以直接编译测试。
//
// 这里集中做三件在真机上极难定位的事：
//   1. 扁平字段串的解码（`|` 分隔 + `¦`/`╲` 转义，见 app_proto.c 的 ap_flat_next）；
//      主机把数组拍平成标量，设备端不解析嵌套 JSON。
//   2. 行模型与选中语义：handle 是设备侧唯一稳定标识（列表顺序会因新消息重排，
//      绝不能用下标跨消息引用会话）。
//   3. session.done 的本地收敛：未选中收到结束 → 未读标记；错误/中断 → 行状态。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_proto.h"   // AP_SESSION_MAX / AP_SESSION_TITLE_BYTES / AP_SSTATE_*

#ifdef __cplusplus
extern "C" {
#endif

// handle 是 2 位十六进制字符串（"01".."0f"），留 1 字节结尾。
#define AP_SESSION_HANDLE_MAX (AP_SESSION_HANDLE_LEN + 1)
#define AP_SESSION_TITLE_MAX  (AP_SESSION_TITLE_BYTES + 1)

/** 行状态。数值用于排序/配色下标，不要随意改。 */
typedef enum {
    AP_SESSION_STATE_UNKNOWN = 0,
    AP_SESSION_STATE_RUNNING,
    AP_SESSION_STATE_WAITING,
    AP_SESSION_STATE_ERROR,
    AP_SESSION_STATE_ABORTED,
    AP_SESSION_STATE_IDLE,
} ap_session_state_t;

typedef struct {
    char handle[AP_SESSION_HANDLE_MAX];
    char title[AP_SESSION_TITLE_MAX];
    uint8_t state;      // ap_session_state_t
    bool unread;
    bool valid;
} ap_session_row_t;

typedef struct {
    ap_session_row_t rows[AP_SESSION_MAX];
    int count;                                  // 0..AP_SESSION_MAX
    char selected[AP_SESSION_HANDLE_MAX];       // 空串 = 未选中
} ap_session_list_t;

typedef struct {
    bool valid;
    char handle[AP_SESSION_HANDLE_MAX];
    char title[AP_SESSION_TITLE_MAX];
    uint8_t state;
    int step;               // -1 = 未知
    int step_total;         // 0 = 未知
    char tool[24];
    uint32_t elapsed_ms;
    char summary[128];
} ap_session_detail_t;

/** 状态字符串 ↔ 枚举（与 constants.js 的 SESSION_STATE 一一对应）。 */
bool ap_session_state_parse(const char *name, uint8_t *out);
const char *ap_session_state_name(uint8_t state);

/** 清空列表（count=0 / selected 空）。 */
void ap_session_list_clear(ap_session_list_t *list);

/**
 * 用 session.list 的扁平字段重建列表。
 *
 * @param handles  "01|02"（字段数即 n；主机保证非空）
 * @param titles   "标题|标题"（最后一项为空时字符串会以 '|' 结尾 —— 解码按 n 补齐）
 * @param states   "running|idle"
 * @param unread   "0|1"
 * @param sel      当前选中 handle（空串 = 无）
 * @return false = 条数超上限 / 参数非法；此时列表被清空，避免半旧半新。
 */
bool ap_session_list_apply(ap_session_list_t *list,
                           const char *handles, const char *titles,
                           const char *states, const char *unread, const char *sel);

int ap_session_find(const ap_session_list_t *list, const char *handle);
const ap_session_row_t *ap_session_row(const ap_session_list_t *list, int index);
const ap_session_row_t *ap_session_selected_row(const ap_session_list_t *list);

/** 设置选中（仅在列表中存在时生效）。返回是否成功。 */
bool ap_session_set_selected(ap_session_list_t *list, const char *handle);

/** 清空选中（如链路重建时）。 */
void ap_session_clear_selected(ap_session_list_t *list);

/**
 * session.done 的本地收敛：把结果映射成行状态；未选中则置未读。
 * result 取值 completed / error / aborted（未知按 completed 处理）。
 */
void ap_session_apply_done(ap_session_list_t *list, const char *handle, const char *result);

void ap_session_detail_clear(ap_session_detail_t *detail);
void ap_session_detail_apply(ap_session_detail_t *detail,
                             const char *handle, const char *title, const char *state,
                             int step, int step_total, const char *tool,
                             uint32_t elapsed_ms, const char *summary);

#ifdef __cplusplus
}
#endif
