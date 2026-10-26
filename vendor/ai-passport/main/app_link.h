// main/app_link.h —— 与 DSH 主机的 BLE 联机。
//
// 设备是 BLE **外设（Peripheral / GATT Server）**，Mac 是中央设备（Central）。
// 方向不能反过来：ESP32-C3 做 Central 会与 Wi-Fi 抢占射频，而 macOS 侧也没有
// 可用的 GATT 服务端实现。详见 docs/01-方案设计.md 的"判断一"。
//
// 本模块只负责"管道"：GATT 服务、广播、分片收发、ACK、心跳。
// 业务语义（任务、审批、余额、语音）在 app_*.c 里，通过回调注册进来。
#pragma once

#include "app_proto.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 链路状态。UI 直接映射这组值显示。
typedef enum {
    AP_LINK_OFF = 0,      // 未启动
    AP_LINK_ADVERTISING, // 广播中，等待主机连接
    AP_LINK_CONNECTED,   // 已连接，握手未完成
    AP_LINK_READY,       // 握手完成，可以收发业务消息
    AP_LINK_FAILED,      // 启动失败
} ap_link_state_t;

// 链路统计，UI 与串口日志用。
typedef struct {
    uint32_t frames_rx;      // 收到的分片数
    uint32_t frames_tx;      // 发出的分片数
    uint32_t messages_rx;    // 完整收到的消息数
    uint32_t messages_tx;    // 完整发出的消息数
    uint32_t gaps;           // 缺片/乱序次数
    uint32_t stale;          // 无归属分片次数
    uint32_t overflow;       // 超缓冲丢弃次数
    uint32_t retries;        // 重传次数
    uint32_t ack_timeouts;   // 重传耗尽（判定链路异常）次数
} ap_link_stats_t;

// 收到一条完整消息时回调。json 是以 NUL 结尾的字符串，只在回调期间有效。
// 返回值无意义；回调应尽快返回，重活交给工作任务。
typedef void (*ap_link_message_cb_t)(const char *json, void *user);

// 链路状态变化回调。
typedef void (*ap_link_state_cb_t)(ap_link_state_t state, void *user);

// 初始化 NimBLE 并以 AP_DEVICE_NAME 广播。可重复调用（先 stop）。
esp_err_t ap_link_start(void);

// 停止广播并释放 NimBLE。断开当前连接。
esp_err_t ap_link_stop(void);

// 当前状态与统计。可从任意任务调用（读的是原子快照）。
ap_link_state_t ap_link_state(void);
void ap_link_stats(ap_link_stats_t *out);

// 注册回调。必须在 ap_link_start() 之前调用（避免启动瞬间丢消息）。
void ap_link_set_message_cb(ap_link_message_cb_t cb, void *user);
void ap_link_set_state_cb(ap_link_state_cb_t cb, void *user);

// 当前连接是否可发业务消息（已连接且握手完成）。
bool ap_link_is_ready(void);

// 已连接主机的信息，用于 UI 显示。任一参数可为 NULL。
// rssi 读失败时写 0；mtu 未协商时写默认值。
void ap_link_peer_info(int8_t *out_rssi, uint16_t *out_mtu);

// 缓存的 RSSI（信号格显示用），连接中每 2 秒刷新一次；未连接返回 AP_LINK_RSSI_NONE。
// 可从任意任务调用（读的是原子快照），UI 刷新随便调不心疼。
#define AP_LINK_RSSI_NONE 127
int ap_link_rssi(void);

// 缓存电量（0-100）。由 app.c 的采样任务定期喂入，hello 握手时随包上报 ——
// BLE 回调里不能跑 I2C/ADC 读电量，所以走"先缓存、再发包"的路子。
// 传负数表示未知（hello 里就不带 batteryPercent 字段）。
void ap_link_set_battery(int percent);

// 发一条控制消息。payload 为 JSON 文本（不含结尾 NUL）。
// 需要 ACK 的消息会被登记以便重传；登记失败仍会发送，只是不重传。
// 返回值：ESP_OK 已发出；ESP_ERR_INVALID_STATE 未就绪；
//         ESP_ERR_INVALID_SIZE 超过单消息上限；ESP_FAIL 发送失败。
esp_err_t ap_link_send_json(const char *json, bool ack_required);

// 发一条语音分片（走 VOICE 通道，不做重传：实时性优先，允许丢片）。
// msg_id 由调用方分配，同一段录音内的分片共用同一个 msg_id。
esp_err_t ap_link_send_voice(uint16_t msg_id, const uint8_t *data, uint16_t len);

// 周期性维护：检查重传超时、判定心跳丢失。必须在工作任务里定期调用
// （app_main 每 100ms 调一次即可）。不要在 BLE 回调里调用。
void ap_link_tick(void);

// 主动断开当前连接（UI 上的"断开"用；广播会重新开始）。
void ap_link_disconnect_peer(void);

#ifdef __cplusplus
}
#endif
