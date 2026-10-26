// main/app_proto.h 的姊妹文件：本文件只放**纯逻辑**的接口。
//
// 之所以把纯逻辑单独拆一个头文件，是为了让 host 侧测试只 include 这一个
// （不牵进 NimBLE / LVGL / FreeRTOS），从而能用普通 cc 编译。
#pragma once

#include "app_proto.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── 接收侧重组 ─────────────────────────────────────────────────────────────

typedef enum {
    AP_RX_INCOMPLETE = 0,      // 收下了，消息还没拼完
    AP_RX_MESSAGE,             // 拼完整了，调用 ap_rx_take 取走
    AP_RX_BUSY,                // 上一条还没被取走，本片丢弃
    AP_RX_GAP,                 // 缺片或乱序，整条已丢弃
    AP_RX_STALE,               // 没有 FIRST 就来的片，无归属
    AP_RX_OVERFLOW,            // 超过接收缓冲上限
    AP_RX_VERSION_MISMATCH,    // 协议版本不一致
    AP_RX_REJECT,              // 参数非法（帧头都不够）
} ap_rx_result_t;

typedef struct {
    uint8_t buf[AP_RX_BUFFER_BYTES]; // 正在拼的消息
    uint8_t msg[AP_RX_BUFFER_BYTES]; // 已拼好、等待取走的消息
    uint16_t len;                    // buf 中已用字节
    uint16_t msg_len;                // msg 中有效字节
    uint16_t msg_id;                 // 已就绪消息的来源 msgId（回 ACK 用）
    uint16_t cur_id;                 // 正在拼的 msgId
    uint8_t next_seq;                // 下一片期望的 seq
    bool assembling;                 // 是否正在拼一条消息
    bool saw_last;                   // 是否见过 LAST 位
    bool ready;                      // msg 是否就绪待取
} ap_rx_t;

void ap_rx_reset(ap_rx_t *rx);

// 喂入一片。返回值语义见 ap_rx_result_t。
ap_rx_result_t ap_rx_push(ap_rx_t *rx, const uint8_t *chunk, uint16_t chunk_len);

// 取走已完成的消息（返回内部缓冲指针，取走后 ready 清零）。
// 返回 NULL 表示当前没有就绪的消息。
const uint8_t *ap_rx_take(ap_rx_t *rx, uint16_t *out_len);

// ── 出站编码 ───────────────────────────────────────────────────────────────

// 发送一个分片的回调：返回 false 表示发送失败，编码随即中止。
typedef bool (*ap_emit_fn)(const uint8_t *frame, uint16_t frame_len, void *user);

// 把一条载荷切成若干分片并逐片回调 emit。
// 返回成功发出的片数；0 表示失败（载荷过大、载荷为 NULL、或 emit 返回 false）。
uint16_t ap_encode(const uint8_t *payload, uint16_t payload_len, uint8_t channel,
                   uint16_t msg_id, bool ack_required, uint16_t chunk_payload,
                   ap_emit_fn emit, void *user);

// ── 出站重传记账 ───────────────────────────────────────────────────────────

// 一个待确认槽位。chunks 指向的分片缓冲由调用方持有并保证在确认前有效
// （app_link 把重传数据放在自己的静态缓冲里，不动态分配）。
typedef struct {
    const uint8_t *chunks[AP_TX_RETRY_CHUNKS];
    uint16_t chunk_lens[AP_TX_RETRY_CHUNKS];
    uint8_t chunk_count;
    uint16_t msg_id;
    uint8_t retries;
    uint32_t deadline_ms;
    bool in_use;
} ap_tx_slot_t;

typedef struct {
    ap_tx_slot_t slots[AP_TX_RETRY_SLOTS];
} ap_tx_t;

void ap_tx_reset(ap_tx_t *tx);

// 登记一条待确认消息。同 msgId 重复登记是幂等的（重传场景）。
// 返回 false 表示槽位已满或参数非法 —— 调用方应照常发送，只是别指望重传。
bool ap_tx_track(ap_tx_t *tx, uint16_t msg_id, const uint8_t *const *chunks,
                 const uint16_t *chunk_lens, uint8_t chunk_count);

// 收到 ACK。返回是否确实清掉了一个在途槽位。
bool ap_tx_ack(ap_tx_t *tx, uint16_t msg_id);

// 找出一个已到重传时刻的槽位。返回 true 时 out_slot 指向它。
bool ap_tx_slot_expired(ap_tx_t *tx, uint32_t now_ms, ap_tx_slot_t **out_slot);

// 记一次重传：retries++ 并顺延超时点。
void ap_tx_retried(ap_tx_slot_t *slot, uint32_t now_ms);

// 该槽位是否已达到重传上限（调用方据此判定链路异常）。
bool ap_tx_slot_give_up(const ap_tx_slot_t *slot);

// 在途（尚未确认）消息数，用于面板/UI 显示。
uint8_t ap_tx_inflight(const ap_tx_t *tx);

// ── 极简 JSON 取值 ─────────────────────────────────────────────────────────
// 只服务于"从下行消息里取几个字段"。输入受控（来自我们自己的 host 插件），
// 因此不追求覆盖任意合法 JSON，但**必须**做到不越界、不返回半截字符串。

// 取字符串字段。成功返回 true 并把值写入 out（超长会被判失败而不是截断）。
bool ap_json_str(const char *json, const char *key, char *out, size_t out_size);

// 取整数字段。小数、指数形式一律判失败（不悄悄丢精度）。
bool ap_json_num(const char *json, const char *key, long *out);

// 取布尔字段。
bool ap_json_bool(const char *json, const char *key, bool *out);

// ── 扁平字段序列解码（协议 v2 的 session.* 载荷）───────────────────────────
// 设备端不解析嵌套数组，会话列表用 `|` 拼成并列字符串。本组函数是主机侧
// lib/protocol/flat.js 的 C 镜像：
//   - 按 `|` 拆分，字段可为空；
//   - 还原转义：`¦`(C2 A6) → `|`，`╲`(E2 95 B2) → `\`；
//   - 每次调用取下一个字段，pos 由调用方保存（初始 0）。
//
// 返回值：true = 取到一个字段；false = 已到末尾（或参数非法）。
// 字段超长时返回 false（宁可判失败也不产生半截字符串）。
bool ap_flat_next(const char *src, size_t *pos, char *out, size_t out_size);

// 统计扁平字符串里的字段数（空串 = 0 个字段，与 ap_flat_next 的迭代次数一致）。
// 用于校验 `n` 与字段数是否匹配；不匹配时整条消息丢弃。
size_t ap_flat_count(const char *src);

#ifdef __cplusplus
}
#endif
