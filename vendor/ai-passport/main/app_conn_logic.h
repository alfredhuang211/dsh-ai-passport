// main/app_conn_logic.h —— 连接呈现与重连退避的纯逻辑（无 NimBLE/LVGL 依赖）。
//
// 拆出来的理由与 app_proto_logic.h 相同：这两段是"错了在真机上才发作"的逻辑 ——
//   · RSSI→格数映射错：信号显示撒谎（用户照着它判断要不要挪位置）；
//   · 重连退避错：弱信号断开后卡死在"永远连不上"（真机返工记录）。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 信号格数（0..4）。RSSI 单位 dBm；未连接由调用方另行处理。 */
int ap_rssi_level(int rssi);

/**
 * 广播重开的退避时长（毫秒）。attempt 从 0 起。
 *
 * 弱信号断开/连接失败后立刻重开广播常撞上 BLE_HS_EBUSY（上一次 GAP 还没结束），
 * 必须**按退避重试而不是放弃** —— 放弃的后果是设备从此隐身，只能重启。
 */
uint32_t ap_adv_retry_delay_ms(int attempt);

#ifdef __cplusplus
}
#endif
