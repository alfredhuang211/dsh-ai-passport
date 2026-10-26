// main/app_session.h —— 设备侧会话模型的应用级访问与动作（协议 v2）。
//
// 纯数据/解码逻辑在 app_session_logic.（可 host 测试）；本头文件只声明
// app.c 持有的运行时模型、显示配置开关，以及"向主机请求/选择"的动作。
// 界面层（app_ui / app_screens）通过这里读模型，不直接碰 app.c 的静态变量。
#pragma once

#include <stdbool.h>

#include "app_session_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 最近一次 session.list 的模型（只读）。 */
const ap_session_list_t *app_session_list(void);

/** 最近一次 session.detail 的模型（只读；valid=false 表示还没收到）。 */
const ap_session_detail_t *app_session_detail(void);

/** 设备显示配置（由主机 config 下发；改后需主机动作为准，不在设备持久化）。 */
bool app_session_mask_titles(void);
bool app_session_show_summary(void);
bool app_session_show_elapsed(void);
void app_session_apply_config(bool mask_titles, bool detail_summary, bool detail_elapsed);

/** 主动请求会话列表（握手完成后调用；也可由界面刷新触发）。 */
void app_session_request_list(void);

/**
 * 选中某个会话：发 session.select（主机回列表并清未读）+ session.detail.req。
 * @return false = handle 非法或发送失败。
 */
bool app_session_select(const char *handle);

/** 请求当前选中会话的详情刷新。 */
void app_session_request_detail(void);

#ifdef __cplusplus
}
#endif
