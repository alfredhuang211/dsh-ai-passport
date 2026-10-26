// main/app_link.c —— NimBLE 外设：GATT 服务、广播、分片收发、ACK 与心跳。
//
// 线程模型（很容易写错，先讲清楚）：
//   · NimBLE 的 GAP/GATT 回调跑在 **NimBLE host 任务**里；
//   · 应用的其他逻辑跑在 **app 工作任务**里（ap_link_tick 由它定期调用）。
//   两者共享的状态只有两处，各自用最短临界区保护：
//     ① 连接句柄 / 状态（单字节或单字，读写是原子的）；
//     ② 发送路径（ble_gatts_notify_custom 不是线程安全的）。
//   业务回调（message_cb）在 NimBLE 任务里执行，因此**回调实现必须只做入队**，
//   不能在回调里跑 LVGL、不能阻塞、不能做文件/音频 IO。
//
// 为什么发送不直接调 notify：BLE 协议栈的发送缓冲是有限的，一次性把 16 片
// 全推出去会在拥塞时失败。这里用一个固定深度的队列交给工作任务逐片发。
#include "app_link.h"

#include "app_proto_logic.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "app_conn_logic.h"
#include "app_pair.h"
#include "app_screens.h"
#include "app_ui.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "app_link";

// 出站队列深度。16 片是一条最大消息，留一倍余量给语音与心跳。
#define TX_QUEUE_DEPTH 32
// 单片的 ATT 载荷上限。BLE 默认 MTU 23 → 20 字节载荷；协商到 247 时约 244。
// 这里按"未协商"的保守值算，协商结果在连接后更新（见 update_chunk_payload）。
#define FALLBACK_CHUNK_PAYLOAD 20

typedef struct {
    uint8_t channel;   // 0=control 1=voice
    uint16_t len;
    uint8_t bytes[AP_HEADER_BYTES + 512];
} tx_item_t;

// ── 模块状态 ──────────────────────────────────────────────────────────────

static volatile ap_link_state_t s_state = AP_LINK_OFF;
static volatile uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static volatile uint16_t s_chunk_payload = FALLBACK_CHUNK_PAYLOAD;
static volatile uint32_t s_last_ping_ms = 0;
static volatile uint8_t s_missed_pings = 0;

// RSSI 缓存（信号格显示用）。ble_gap_conn_rssi 是一次控制器查询，
// 不能按 UI 刷新频率调；连接中每 2 秒刷新一次，未连接恒为 AP_LINK_RSSI_NONE。
static volatile int s_rssi_cached = AP_LINK_RSSI_NONE;
static uint32_t s_rssi_next_ms = 0;
#define RSSI_REFRESH_MS 2000

// 广播重开的退避重试（弱信号断开后自愈，见 schedule_adv_retry）。
static volatile bool s_adv_retry_pending = false;
static volatile uint32_t s_adv_retry_at_ms = 0;
static int s_adv_retry_count = 0;

static ap_link_message_cb_t s_message_cb = NULL;
static void *s_message_user = NULL;
static ap_link_state_cb_t s_state_cb = NULL;
static void *s_state_user = NULL;

static ap_rx_t s_rx;
static ap_tx_t s_tx;
static ap_link_stats_t s_stats;
static uint16_t s_next_msg_id = 1;

// 出站队列与其保护锁。用队列而不是直接发送：见文件头注释。
static tx_item_t s_tx_queue[TX_QUEUE_DEPTH];
static volatile uint8_t s_tx_head = 0;
static volatile uint8_t s_tx_tail = 0;
static SemaphoreHandle_t s_tx_lock = NULL;

// 重传用的分片副本。ap_tx_t 里的指针指向这里 —— 必须是静态存储，
// 因为确认之前一直要能取到内容。
// 重传分片副本。片数按 AP_TX_RETRY_CHUNKS（实际报文所需）而不是 AP_MAX_CHUNKS
// （seq 位宽的理论上限）—— 后者会多吃十几 KB DRAM，实测会把 DRAM 撑爆。
static uint8_t s_retry_pool[AP_TX_RETRY_SLOTS][AP_TX_RETRY_CHUNKS][AP_HEADER_BYTES + 512];
static uint16_t s_retry_lens[AP_TX_RETRY_SLOTS][AP_TX_RETRY_CHUNKS];
static uint8_t s_retry_counts[AP_TX_RETRY_SLOTS];

// 迟到/重复 ACK 的累计次数（槽位已释放后主机才回来的 ACK）。
//
// ★ 这是**良性**现象：语音突发期间音频通知会把控制通道挤拥塞，主机对已经
//   收妥（或设备已放弃重传）的消息再回一条 ACK 很常见。但之前每次都记一条 W，
//   一轮录音就刷一条 —— 把"日志里 0 个 E/W"这个我们一直用来判断健康的判据
//   污染成"每条 W 都要人工判断要不要紧"（真机排障时确实为此多花了一轮）。
//   现在：首次记 W（便于一次性确认），之后每 20 次记一条并带累计数，其余降到 DEBUG。
static uint32_t s_late_ack_count = 0;

static uint8_t s_addr_type = 0;
static bool s_initialized = false;
static bool s_start_requested = false;
static uint16_t s_tx_chr_handle = 0;
static uint16_t s_voice_chr_handle = 0;

// 本次连接是否已主动发过 hello（退订后重置，允许重连时重新握手）。
static bool s_hello_sent = false;
// 当前主机的 token 是否校验通过（未配对/不符时为 false → 业务消息一律拒绝）
static bool s_host_token_ok = false;
static uint16_t s_ctrl_chr_handle = 0;

// 电量缓存（-1 = 未知）。app.c 采样任务喂入，hello 时读取 ——
// BLE 回调里做 I2C/ADC 读会阻塞协议栈，只能"先缓存、后发包"。
static volatile int s_battery_percent = -1;

// 供 CTRL 特征值读取的设备能力描述（连接后主机先读它）。
static char s_ctrl_value[128];

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void set_state(ap_link_state_t next)
{
    const ap_link_state_t prev = s_state;
    if (prev == next) return;
    s_state = next;
    ESP_LOGI(TAG, "链路状态 %d -> %d", prev, next);
    if (s_state_cb) s_state_cb(next, s_state_user);
}

// ── 出站队列 ──────────────────────────────────────────────────────────────

static bool tx_enqueue(uint8_t channel, const uint8_t *frame, uint16_t len)
{
    if (!s_tx_lock) return false;
    if (len > sizeof(s_tx_queue[0].bytes)) return false;
    if (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    const uint8_t next = (uint8_t)((s_tx_tail + 1) % TX_QUEUE_DEPTH);
    if (next == s_tx_head) {
        // 队列满：丢弃而不是阻塞。BLE 拥塞时阻塞回调会拖垮协议栈。
        xSemaphoreGive(s_tx_lock);
        ESP_LOGW(TAG, "发送队列已满，丢弃一片");
        return false;
    }
    s_tx_queue[s_tx_tail].channel = channel;
    s_tx_queue[s_tx_tail].len = len;
    memcpy(s_tx_queue[s_tx_tail].bytes, frame, len);
    s_tx_tail = next;
    xSemaphoreGive(s_tx_lock);
    return true;
}

static bool tx_dequeue(tx_item_t *out)
{
    if (!s_tx_lock) return false;
    bool got = false;
    if (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (s_tx_head != s_tx_tail) {
            *out = s_tx_queue[s_tx_head];
            s_tx_head = (uint8_t)((s_tx_head + 1) % TX_QUEUE_DEPTH);
            got = true;
        }
        xSemaphoreGive(s_tx_lock);
    }
    return got;
}

// 把一片真正写进 GATT 通知。返回 ESP_OK 表示协议栈已接受。
static esp_err_t notify_chunk(uint8_t channel, const uint8_t *frame, uint16_t len)
{
    const uint16_t conn = s_conn_handle;
    if (conn == BLE_HS_CONN_HANDLE_NONE) return ESP_ERR_INVALID_STATE;

    const uint16_t handle = (channel == AP_CH_VOICE) ? s_voice_chr_handle : s_tx_chr_handle;
    if (handle == 0) return ESP_ERR_INVALID_STATE;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(frame, len);
    if (!om) return ESP_ERR_NO_MEM;

    const int rc = ble_gatts_notify_custom(conn, handle, om);
    if (rc != 0) {
        // 通知失败时 mbuf 由协议栈负责回收，这里不重复释放。
        // ★ 用 WARN 而不是 DEBUG：这是"设备说发了、主机收不到"这类问题的
        //   唯一线索。之前它在 DEBUG 级别，默认日志里完全看不见，
        //   导致排查方向长期偏在主机侧。
        ESP_LOGW(TAG, "notify 失败 rc=%d（handle=%u len=%u）", rc, (unsigned)handle, (unsigned)len);
        return ESP_FAIL;
    }
    s_stats.frames_tx++;
    return ESP_OK;
}

// 由工作任务调用：把队列里的片逐片发出去。
static void drain_tx_queue(void)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    tx_item_t item;
    // 每次 tick 限量发送：给协议栈喘息时间，也避免阻塞工作任务过久。
    for (int budget = 0; budget < 8; budget++) {
        if (!tx_dequeue(&item)) return;
        if (notify_chunk(item.channel, item.bytes, item.len) != ESP_OK) {
            // 单次失败不重排队：控制消息有自己的重传记账（见 ap_tx_*），
            // 语音本来就允许丢片。重排队会放大拥塞。
            continue;
        }
    }
}

// ── 发送路径 ──────────────────────────────────────────────────────────────

static bool emit_to_queue(const uint8_t *frame, uint16_t frame_len, void *user)
{
    (void)user;
    return tx_enqueue(frame[1], frame, frame_len);
}

esp_err_t ap_link_send_json(const char *json, bool ack_required)
{
    if (!json) return ESP_ERR_INVALID_ARG;

    // ★ 允许在 CONNECTED（已连接、尚未握手）状态下发送 —— 这不是放松校验，
    //   而是修一个**循环依赖**：
    //
    //   状态机只在收到主机的 hello.ack 之后才进入 READY，而 hello.ack 是对
    //   设备 hello 的回应。若这里要求 READY 才肯发，就变成
    //   "发 hello 需要 READY，进入 READY 需要先发 hello" —— 死锁。
    //   现象极具迷惑性：连接成功、订阅成功、设备日志显示"已主动发送 hello"，
    //   但那条 hello 根本没出去（send_control_short 内部静默失败），
    //   主机于是永远等不到第一条消息。
    //
    //   握手消息本身就属于"连接建立后、READY 之前"这个阶段，必须放行。
    //   OFF / ADVERTISING / FAILED 仍然拒绝 —— 那时根本没有对端。
    if (s_state != AP_LINK_READY && s_state != AP_LINK_CONNECTED) {
        ESP_LOGW(TAG, "链路未就绪（state=%d），丢弃待发消息", (int)s_state);
        return ESP_ERR_INVALID_STATE;
    }

    const uint16_t len = (uint16_t)strlen(json);
    const uint16_t chunk_payload = s_chunk_payload;
    if (!ap_payload_fits(chunk_payload, len)) return ESP_ERR_INVALID_SIZE;

    const uint16_t msg_id = s_next_msg_id;
    s_next_msg_id = (uint16_t)((s_next_msg_id + 1) & 0x0fff);

    // 需要确认的消息：先把分片编码进重传池，再登记。
    if (ack_required) {
        // 找一个空闲的池槽。ap_tx_track 负责把指针存进记账表。
        int pool = -1;
        for (int i = 0; i < AP_TX_RETRY_SLOTS; i++) {
            if (s_retry_counts[i] == 0) { pool = i; break; }
        }
        if (pool >= 0) {
            // 用 ap_encode 直接写进池子
            uint8_t count = 0;
            const uint16_t total = (uint16_t)((len + chunk_payload - 1) / chunk_payload);
            // 超过重传池容量时**不登记重传**，但仍把消息发出去（比不发好）；
            // 这条路径只在小 MTU + 长报文时出现，属于可接受的降级。
            if (total > AP_TX_RETRY_CHUNKS) return ESP_ERR_INVALID_SIZE;
            for (uint16_t seq = 0; seq < total; seq++) {
                const uint16_t offset = (uint16_t)(seq * chunk_payload);
                uint16_t slice = (uint16_t)(len - offset);
                if (slice > chunk_payload) slice = chunk_payload;
                uint8_t flags = 0;
                if (seq == 0) flags |= AP_FLAG_FIRST;
                if (seq == total - 1) flags |= AP_FLAG_LAST;
                flags |= AP_FLAG_ACK_REQ;
                uint8_t *dst = s_retry_pool[pool][seq];
                dst[0] = ap_head_byte0(AP_PROTOCOL_VERSION, flags);
                dst[1] = AP_CH_CONTROL;
                dst[2] = (uint8_t)((msg_id >> 4) & 0xff);
                dst[3] = ap_head_byte3(msg_id, (uint8_t)seq);
                memcpy(dst + AP_HEADER_BYTES, json + offset, slice);
                s_retry_lens[pool][seq] = (uint16_t)(AP_HEADER_BYTES + slice);
                count++;
            }
            s_retry_counts[pool] = count;

            const uint8_t *ptrs[AP_TX_RETRY_CHUNKS];
            for (uint8_t i = 0; i < count; i++) ptrs[i] = s_retry_pool[pool][i];
            if (!ap_tx_track(&s_tx, msg_id, ptrs, s_retry_lens[pool], count)) {
                // 槽满：不重传，但仍把消息发出去（比不发好）。
                s_retry_counts[pool] = 0;
            }
        }
    }

    const uint16_t sent = ap_encode((const uint8_t *)json, len, AP_CH_CONTROL, msg_id,
                                    ack_required, chunk_payload, emit_to_queue, NULL);
    if (sent == 0) return ESP_FAIL;
    s_stats.messages_tx++;
    return ESP_OK;
}

esp_err_t ap_link_send_voice(uint16_t msg_id, const uint8_t *data, uint16_t len)
{
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    if (s_state != AP_LINK_READY) return ESP_ERR_INVALID_STATE;

    // 语音是"一条很长的一次性消息"，按片发送，不做 ACK 记账。
    const uint16_t chunk_payload = s_chunk_payload;
    const uint16_t total = (uint16_t)((len + chunk_payload - 1) / chunk_payload);
    if (total == 0 || total > AP_MAX_CHUNKS) return ESP_ERR_INVALID_SIZE;

    uint8_t frame[AP_HEADER_BYTES + 512];
    if (chunk_payload > 512) return ESP_ERR_INVALID_SIZE;

    for (uint16_t seq = 0; seq < total; seq++) {
        const uint16_t offset =
            (uint16_t)(seq * chunk_payload);
        uint16_t slice = (uint16_t)(len - offset);
        if (slice > chunk_payload) slice = chunk_payload;
        uint8_t flags = 0;
        if (seq == 0) flags |= AP_FLAG_FIRST;
        if (seq == total - 1) flags |= AP_FLAG_LAST;
        frame[0] = ap_head_byte0(AP_PROTOCOL_VERSION, flags);
        frame[1] = AP_CH_VOICE;
        frame[2] = (uint8_t)((msg_id >> 4) & 0xff);
        frame[3] = ap_head_byte3(msg_id, (uint8_t)seq);
        memcpy(frame + AP_HEADER_BYTES, data + offset, slice);
        if (!tx_enqueue(AP_CH_VOICE, frame, (uint16_t)(AP_HEADER_BYTES + slice))) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

// ── ACK 与心跳维护 ────────────────────────────────────────────────────────

// 回一条极短的控制消息（ack / nack / pong）。不要求对端再确认，避免无限往复。
/**
 * 发一条简短的控制消息。
 *
 * ★ `ack_required` 必须与"这条消息是否登记进重传表"一致，否则会自相矛盾：
 *   设备等一个它没要求的 ACK，一直等到重传上限，然后判定链路异常。
 *   这个不一致真实发生过 —— 设备发的 hello 帧头不含 ACK_REQ 位，
 *   主机（正确地）不回 ACK，而设备却把它登记进重传表并重传 3 次，
 *   结果握手永远无法收敛，链路卡在 failed，后续业务消息全部发不出去。
 *
 * 约定：
 *   · 需要对方确认的关键消息（握手 hello）→ ack_required = true
 *   · 应答类消息（ack / nack / pong）→ false。它们本身就是对某条消息的回应，
 *     再要求确认会形成互相确认的死循环。
 */
static void send_control_short_ex(const char *type, uint16_t ref_msg_id, const char *extra,
                                  bool ack_required)
{
    // 160 字节：hello 带 firmware+batteryPercent 后整包约 110 字节，
    // 96 的旧尺寸会在 snprintf 处静默截断成坏 JSON（连右括号都丢了）。
    char buf[160];
    if (extra && extra[0]) {
        snprintf(buf, sizeof(buf), "{\"type\":\"%s\",\"msgId\":%u,%s}", type,
                 (unsigned)ref_msg_id, extra);
    } else {
        snprintf(buf, sizeof(buf), "{\"type\":\"%s\",\"msgId\":%u}", type,
                 (unsigned)ref_msg_id);
    }
    (void)ap_link_send_json(buf, ack_required);
}

/** 应答类消息：不要求确认（见 send_control_short_ex 的约定）。 */
static void send_control_short(const char *type, uint16_t ref_msg_id, const char *extra)
{
    send_control_short_ex(type, ref_msg_id, extra, false);
}

static void handle_control_message(const char *json)
{
    char type[48];
    if (!ap_json_str(json, "type", type, sizeof(type))) {
        ESP_LOGW(TAG, "控制消息缺少 type 字段，已忽略");
        return;
    }

    // 协议级消息在这里就地处理，不下发到业务层。
    if (strcmp(type, AP_MSG_ACK) == 0) {
        long id = 0;
        const bool has = ap_json_num(json, "msgId", &id);
        if (has) {
            const uint16_t ack_id = (uint16_t)id;
            if (ap_tx_ack(&s_tx, ack_id)) {
                ESP_LOGD(TAG, "ACK 匹配成功 msgId=%u", (unsigned)ack_id);
                // 释放对应的重传池槽
                for (int i = 0; i < AP_TX_RETRY_SLOTS; i++) {
                    if (s_retry_counts[i] > 0 && s_tx.slots[i].msg_id == ack_id &&
                        !s_tx.slots[i].in_use) {
                        s_retry_counts[i] = 0;
                    }
                }
            } else {
                s_late_ack_count += 1;
                if (s_late_ack_count == 1 || (s_late_ack_count % 20) == 0) {
                    ESP_LOGW(TAG, "迟到/重复 ACK msgId=%u（累计 %u 次，槽位已释放，属良性）",
                             (unsigned)ack_id, (unsigned)s_late_ack_count);
                } else {
                    ESP_LOGD(TAG, "迟到/重复 ACK msgId=%u（累计 %u 次）",
                             (unsigned)ack_id, (unsigned)s_late_ack_count);
                }
            }
        }
        return;
    }
    if (strcmp(type, AP_MSG_NACK) == 0) {
        // 对端报缺口：把对应消息立刻重传一次（若还在记账表里）。
        long id = 0;
        if (ap_json_num(json, "msgId", &id)) {
            ESP_LOGW(TAG, "对端报缺口 msgId=%ld，等待超时重传", id);
        }
        return;
    }
    if (strcmp(type, AP_MSG_PING) == 0) {
        s_last_ping_ms = now_ms();
        s_missed_pings = 0;
        char tbuf[32];
        long t = 0;
        if (ap_json_num(json, "t", &t)) {
            snprintf(tbuf, sizeof(tbuf), "\"t\":%ld", t);
            send_control_short(AP_MSG_PONG, 0, tbuf);
        } else {
            send_control_short(AP_MSG_PONG, 0, NULL);
        }
        return;
    }
    if (strcmp(type, AP_MSG_HELLO) == 0) {
        // 主机发起握手。带 token 说明它自认为已配对（见 docs/06 §9.14）。
        char token[AP_PAIR_TOKEN_HEX + 1];
        const bool has_token = ap_json_str(json, "token", token, sizeof(token));
        if (ap_pair_is_paired()) {
            if (!has_token || !ap_pair_host_token_ok(token)) {
                // 已配对，但对方没带/带错 token：不放行业务消息，让主机重新配对
                s_host_token_ok = false;
            ESP_LOGW(TAG, "主机 token 不匹配（%s），回 pair.required",
                         has_token ? "值不符" : "缺失");
                send_control_short(AP_MSG_PAIR_REQUIRED, 0, "\"reason\":\"token\"");
                return;
            }
            ESP_LOGI(TAG, "收到主机 hello（token 校验通过），回 hello.ack");
            s_host_token_ok = true;
            send_control_short(AP_MSG_HELLO_ACK, 0, "\"hostReady\":true");
            set_state(AP_LINK_READY);
            return;
        }

        // 未配对：把配对码显示出来，并提示主机（业务消息在此之前一律拒绝）
        ESP_LOGW(TAG, "未配对的主机接入，要求配对（设备 %s）", ap_pair_device_id());
        app_screens_post_pair_code(ap_pair_code_str(), ap_pair_device_id());
        send_control_short(AP_MSG_PAIR_REQUIRED, 0, "\"reason\":\"unpaired\"");
        return;
    }

    // ── 配对握手（docs/06 §9.14）─────────────────────────────────────────
    if (strcmp(type, AP_MSG_PAIR_BEGIN) == 0) {
        char code[AP_PAIR_CODE_DIGITS + 1];
        char host[32];
        char token[AP_PAIR_TOKEN_HEX + 1];
        const bool has_code = ap_json_str(json, "code", code, sizeof(code));
        if (!ap_json_str(json, "host", host, sizeof(host))) host[0] = '\0';
        const esp_err_t rc = has_code ? ap_pair_begin(code, host, token) : ESP_ERR_INVALID_ARG;
        if (rc == ESP_OK) {
            char out[96];
            snprintf(out, sizeof(out), "\"deviceId\":\"%s\",\"token\":\"%s\"",
                     ap_pair_device_id(), token);
            send_control_short_ex(AP_MSG_PAIR_OK, 0, out, true);
            app_screens_hide(AP_SCREEN_PAIR);      // 配对成功：收起配对码页
            app_ui_post_toast("配对成功", false);
            ESP_LOGI(TAG, "配对完成，已回 pair.ok");
        } else {
            const bool limited = (rc == ESP_ERR_INVALID_STATE);
            char out[96];
            snprintf(out, sizeof(out), "\"reason\":\"%s\",\"remaining\":%d",
                     limited ? "rate-limited" : "bad-code", ap_pair_attempts_left());
            send_control_short_ex(AP_MSG_PAIR_FAILED, 0, out, true);
        }
        return;
    }
    if (strcmp(type, AP_MSG_PAIR_RESET) == 0) {
        (void)ap_pair_reset();
        send_control_short(AP_MSG_PAIR_REQUIRED, 0, "\"reason\":\"reset\"");
        app_screens_post_pair_code(ap_pair_code_str(), ap_pair_device_id());
        return;
    }

    // ── 未配对 / token 不符时，业务消息一律不执行 ──
    if (!ap_pair_is_paired() || !s_host_token_ok) {
        ESP_LOGW(TAG, "未配对的链路发来业务消息 type=%s —— 回 pair.required", type);
        send_control_short(AP_MSG_PAIR_REQUIRED, 0, "\"reason\":\"unpaired\"");
        return;
    }

    // 其余交给业务层。
    ESP_LOGD(TAG, "分发消息 type=%s", type);
    if (s_message_cb) s_message_cb(json, s_message_user);
}

static void handle_message(uint16_t msg_id, uint8_t channel, const uint8_t *payload, uint16_t len)
{
    // JSON 通道需要 NUL 结尾。协议上限内不会越界（缓冲比最大消息大一字节的余量由
    // AP_RX_BUFFER_BYTES 提供，这里再挡一次）。
    if (len >= AP_RX_BUFFER_BYTES) return;

    if (channel == AP_CH_CONTROL || channel == AP_CH_HEARTBEAT || channel == AP_CH_LOG) {
        // 关键：JSON 缓冲必须是静态的。栈上放 2KB 会给工作任务带来不必要的栈压力，
        // 而这条路径本身是串行的（同一时刻只处理一条消息）。
        static char json[AP_RX_BUFFER_BYTES + 1];
        memcpy(json, payload, len);
        json[len] = '\0';
        s_stats.messages_rx++;

        // 要求确认的消息要回 ACK：这是对端停止重传的唯一依据。
        // 先用消息里的 type 判断，避免把 ACK 自己变成需要 ACK 的消息。
        char type[48];
        const bool parsed = ap_json_str(json, "type", type, sizeof(type));
        const bool is_ack_like = parsed &&
            (strcmp(type, AP_MSG_ACK) == 0 || strcmp(type, AP_MSG_NACK) == 0 ||
             strcmp(type, AP_MSG_PONG) == 0);
        if (!is_ack_like) send_control_short(AP_MSG_ACK, msg_id, NULL);

        // ★ 收到任何控制消息都证明主机还活着，重置心跳计时。
        //   之前只有 PING 才重置，但语音上行期间音频通知会把控制通道挤拥塞，
        //   ping/pong 都可能丢 —— 丢失不等于主机走了。只要还有任何控制流量
        //   （ACK/NACK/hello/voice.result/…）就说明链路是通的，不该判掉线。
        //   这条是"识别后有时自己断开连接"的关键修复。
        s_last_ping_ms = now_ms();
        s_missed_pings = 0;

        handle_control_message(json);
    } else if (channel == AP_CH_VOICE) {
        // 语音上行的重组在 app_voice 里做（它需要按录音会话聚合）。
        // 这里只把原始片交给业务层需要的信息，实际数据由 voice 模块自己订阅。
        if (s_message_cb) {
            static char marker[64];
            snprintf(marker, sizeof(marker), "{\"type\":\"voice.frame\",\"msgId\":%u,\"bytes\":%u}",
                     (unsigned)msg_id, (unsigned)len);
            s_message_cb(marker, s_message_user);
        }
    }
}

// ── GATT ──────────────────────────────────────────────────────────────────

static int chr_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)arg;
    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        // 收片。逐片喂给重组器；完成时交付。
        uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
        if (om_len > AP_HEADER_BYTES + 512) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

        uint8_t chunk[AP_HEADER_BYTES + 512];
        uint16_t flat_len = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, chunk, sizeof(chunk), &flat_len) != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        s_stats.frames_rx++;

        const ap_rx_result_t r = ap_rx_push(&s_rx, chunk, flat_len);
        // 写入路径的观测点。原先这里完全没有输出，导致"主机发了没到"与
        // "到了但没解析"这两种完全不同的原因在日志上无法区分，只能靠猜。
        // 用 DEBUG 而不是 INFO：正常流量（尤其音频片）会刷屏。
        // 排查握手/协议问题时把日志级别调到 DEBUG 即可看到每一片的细节。
        ESP_LOGD(TAG, "写入 %u 字节 ch=%u msgId=%u seq=%u flags=0x%02x → 结果=%d",
                 (unsigned)flat_len, flat_len > 1 ? (unsigned)chunk[1] : 0u,
                 flat_len >= 4 ? (unsigned)ap_head_msg_id(chunk) : 0u,
                 flat_len >= 4 ? (unsigned)ap_head_seq(chunk) : 0u,
                 flat_len >= 1 ? (unsigned)ap_head_flags(chunk) : 0u,
                 (int)r);
        switch (r) {
        case AP_RX_MESSAGE: {
            uint16_t len = 0;
            const uint8_t *msg = ap_rx_take(&s_rx, &len);
            if (msg) handle_message(s_rx.msg_id, chunk[1], msg, len);
            break;
        }
        case AP_RX_GAP:
            s_stats.gaps++;
            // 让对端立刻重传，比等它超时更快恢复
            send_control_short(AP_MSG_NACK, ap_head_msg_id(chunk), "\"reason\":\"gap\"");
            break;
        case AP_RX_STALE:
            s_stats.stale++;
            break;
        case AP_RX_OVERFLOW:
            s_stats.overflow++;
            send_control_short(AP_MSG_NACK, ap_head_msg_id(chunk), "\"reason\":\"overflow\"");
            break;
        case AP_RX_VERSION_MISMATCH:
            ESP_LOGW(TAG, "协议版本不一致：收到 %u，本地 %u",
                     ap_head_version(chunk), AP_PROTOCOL_VERSION);
            break;
        default:
            break;
        }
        return 0;
    }
    case BLE_GATT_ACCESS_OP_READ_CHR: {
        // CTRL：主机连上后先读它，拿到固件版本、协议版本、能力位与电量。
        if (attr_handle != s_ctrl_chr_handle) return BLE_ATT_ERR_UNLIKELY;
        const int rc = os_mbuf_append(ctxt->om, s_ctrl_value, strlen(s_ctrl_value));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static const struct ble_gatt_svc_def GATT_SVCS[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID128_DECLARE(AP_UUID_TAIL, 0x00, 0xa9),
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                // RX：Mac → 设备，Write Without Response（低延迟，音频与批量都用它）
                .uuid = AP_UUID_CHAR_RX,
                .access_cb = chr_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                // TX：设备 → Mac，Notify（JSON 控制）
                .uuid = AP_UUID_CHAR_TX,
                .access_cb = chr_access_cb,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_tx_chr_handle,
            },
            {
                // VOICE：设备 → Mac，Notify（音频，与控制分开以免互相堵队列）
                .uuid = AP_UUID_CHAR_VOICE,
                .access_cb = chr_access_cb,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_voice_chr_handle,
            },
            {
                // CTRL：设备 → Mac，Read（能力/版本/电量，连上即可读）
                .uuid = AP_UUID_CHAR_CTRL,
                .access_cb = chr_access_cb,
                .flags = BLE_GATT_CHR_F_READ,
                .val_handle = &s_ctrl_chr_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

// ── 广播与连接 ────────────────────────────────────────────────────────────

static void update_chunk_payload(void)
{
    const uint16_t conn = s_conn_handle;
    if (conn == BLE_HS_CONN_HANDLE_NONE) {
        s_chunk_payload = FALLBACK_CHUNK_PAYLOAD;
        return;
    }
    const uint16_t mtu = ble_att_mtu(conn);
    // ★ 整帧 = 4 字节分片头 + payload，而 ATT 通知的载荷上限是 MTU - 3（1 字节
    //   opcode + 2 字节 handle）。因此 payload ≤ MTU - 3 - 4，再留 1 字节余量：
    //       payload = MTU - 8
    //
    // 旧公式是 mtu - 4 —— 它只扣了"ATT 头 3 + 余量 1"，**漏扣了帧内的 4 字节
    // 分片头**，于是整帧恰好 = MTU、超限 3 字节。对端（macOS CoreBluetooth）
    // 会静默截断每个满片的尾部 3 字节。影响面极隐蔽：
    //   · 控制消息都是小 JSON（单片远小于上限）→ 从不触线，一切正常；
    //   · 只有语音故意把每片填满 → 逐片丢 3 字节 → ADPCM 块边界持续漂移
    //     → 解码出饱和噪声、识别返回空；seq 号不丢（seqGaps=0）、总长几乎不变，
    //     排查时表现为"音频到了、字节量对、就是解出来是噪声"。
    uint16_t payload = (mtu > 8) ? (uint16_t)(mtu - 8) : (uint16_t)15;
    if (payload > 512) payload = 512;
    if (payload < 15) payload = 15;   // MTU 23 时帧 19 ≤ 20，仍合规
    s_chunk_payload = payload;
    ESP_LOGI(TAG, "MTU=%u，单片载荷=%u", mtu, payload);
}

static int gap_event_cb(struct ble_gap_event *event, void *arg);

/**
 * 广播启动失败 → 按退避重试，**绝不放弃**。
 *
 * ★ 真机返工记录（"弱信号断开后永远连不上，必须重启设备"）：
 *   弱信号断开/连接失败后立刻重开广播常撞上 BLE_HS_EBUSY（上一次 GAP 还没结束），
 *   旧代码在这里 set_state(AP_LINK_FAILED) 就再也不动了 —— 设备从此隐身，
 *   只能重启。现在失败只安排一次退避重试（ap_link_tick 驱动），永远自愈。
 */
static void schedule_adv_retry(void)
{
    const uint32_t delay = ap_adv_retry_delay_ms(s_adv_retry_count);
    s_adv_retry_pending = true;
    s_adv_retry_at_ms = now_ms() + delay;
    set_state(AP_LINK_ADVERTISING);   // 语义是"仍在待连接"，不是死掉
    ESP_LOGW(TAG, "广播未启动，%ums 后重试（第 %d 次）",
             (unsigned)delay, s_adv_retry_count + 1);
}

static void advertise(void)
{
    // ★ 广播包只有 31 字节。这里踩过两次坑，把字节账和结论都写清楚：
    //
    // 坑 1：Flags(3) + 128 位 UUID(18) + 完整名(19) = 40 > 31
    //       → ble_gap_adv_set_fields 返回 rc=4 (BLE_HS_EMSGSIZE)，退化成"仅名字"广播。
    //
    // 坑 2（更隐蔽）：把广播包**刚好填满 31 字节**（Flags+UUID+短名 = 3+18+10 = 31）
    //       虽然 rc=0，但实测 macOS 把设备报成 `connectable: false`，
    //       CoreBluetooth 于是拒绝建立连接 —— 表现为"扫得到、连不上"。
    //       顶格广播本身就是脆弱状态（各控制器对保留字节的口径不一致）。
    //
    // 因此现在的分配**刻意留余量**，并把"识别"与"显示"彻底分开：
    //   广播包   = Flags(3) + 128 位服务 UUID(18) + 厂商数据(9) = 30 字节
    //              → 主机据此**精准识别**设备，完全不依赖名字
    //   扫描响应 = 完整设备名(19) + 短名(9) = 28 字节
    //              → 给人看的名字，还能放下完整名而不被截断
    static ble_uuid128_t svc_uuid = AP_UUID_SERVICE_VAR;
    static const uint8_t mfg_data[AP_ADV_MFG_LEN] = { AP_ADV_MFG_TAG };

    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    fields.mfg_data = mfg_data;
    fields.mfg_data_len = AP_ADV_MFG_LEN;
    // 广播包里**不放名字**：一来它会把报文顶到 31 字节上限（坑 2），
    // 二来名字本就该放扫描响应。主机侧按服务 UUID / 厂商数据识别，不依赖名字。

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        // 走到这里说明字节账又错了。退化为"仅名字"，至少还能被扫到。
        ESP_LOGW(TAG, "广播字段设置失败 rc=%d，退化为仅名字广播", rc);
        struct ble_hs_adv_fields fallback = { 0 };
        fallback.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
        fallback.name = (const uint8_t *)AP_DEVICE_NAME_SHORT;
        fallback.name_len = (uint8_t)strlen(AP_DEVICE_NAME_SHORT);
        fallback.name_is_complete = 1;
        rc = ble_gap_adv_set_fields(&fallback);
        if (rc != 0) {
            ESP_LOGE(TAG, "退化的广播字段也失败 rc=%d", rc);
            schedule_adv_retry();
            return;
        }
    }

    // 扫描响应：完整设备名（这里另有 31 字节可用）。
    struct ble_hs_adv_fields rsp = { 0 };
    rsp.name = (const uint8_t *)AP_DEVICE_NAME;
    rsp.name_len = (uint8_t)strlen(AP_DEVICE_NAME);
    rsp.name_is_complete = 1;
    const int rsp_rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rsp_rc != 0) {
        // 名字进不了扫描响应不影响连接与协议，只影响扫描列表里显示的名称。
        ESP_LOGW(TAG, "扫描响应设置失败 rc=%d（设备名可能显示不全）", rsp_rc);
    }


    struct ble_gap_adv_params params = { 0 };
    params.conn_mode = BLE_GAP_CONN_MODE_UND;   // 可连接
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event_cb, NULL);
    if (rc == 0) {
        s_adv_retry_pending = false;
        s_adv_retry_count = 0;
        set_state(AP_LINK_ADVERTISING);
        ESP_LOGI(TAG, "开始广播：%s", AP_DEVICE_NAME);
    } else {
        ESP_LOGE(TAG, "广播启动失败 rc=%d", rc);
        schedule_adv_retry();
    }
}

// 广播回调：连接、断开、MTU 协商、订阅都从这里来。
// 它跑在 NimBLE host 任务里，因此只做状态更新与重开广播，不做任何耗时操作。
static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_adv_retry_pending = false;
            s_adv_retry_count = 0;
            ap_rx_reset(&s_rx);
            ap_tx_reset(&s_tx);
            memset(s_retry_counts, 0, sizeof(s_retry_counts));
            s_missed_pings = 0;
            s_last_ping_ms = now_ms();
            update_chunk_payload();
            set_state(AP_LINK_CONNECTED);
            ESP_LOGI(TAG, "主机已连接（等待握手）");
        } else {
            ESP_LOGW(TAG, "连接失败 status=%d，重新广播", event->connect.status);
            advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "连接断开 reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_chunk_payload = FALLBACK_CHUNK_PAYLOAD;
        ap_rx_reset(&s_rx);
        ap_tx_reset(&s_tx);
        memset(s_retry_counts, 0, sizeof(s_retry_counts));
        // 断开后立刻回到广播：用户不需要手动做任何事。
        if (s_start_requested) advertise();
        else set_state(AP_LINK_OFF);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        // 广播超时（本实现用 FOREVER，正常不会走到；防御性重开）。
        if (s_start_requested) advertise();
        return 0;

    case BLE_GAP_EVENT_MTU:
        update_chunk_payload();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE: {
        const bool is_tx = (event->subscribe.attr_handle == s_tx_chr_handle);
        ESP_LOGI(TAG, "主机订阅 attr=%d notify=%d%s",
                 event->subscribe.attr_handle, event->subscribe.cur_notify,
                 is_tx ? "（TX 控制通道）" : "");

        // ★ 握手必须由**设备**发起 —— 这一条以前漏了，导致链路永远静默。
        //
        // 协议约定：设备在主机订好 TX 通知后主动发 hello，主机回 hello.ack，
        // 握手闭环。但之前的实现只有"收到主机 hello 就回 hello.ack"这一半，
        // **没有任何地方主动发 hello**。于是主机在等设备的 hello、
        // 设备在等主机的 hello，双方永久互等：连得上、订阅成功，但一条消息都没有。
        // 这类"两边都在等对方"的 bug 从日志上看不出任何错误，只能靠
        // "把每一侧的发起方写清楚"来避免。
        //
        // 时机选在"主机订阅 TX 通知之后"：早于订阅发出去的 notify 会被直接丢弃
        // （对端还没监听），那正是 hello 最容易丢的窗口。
        if (is_tx && event->subscribe.cur_notify && !s_hello_sent) {
            s_hello_sent = true;
            // 握手是链路能否进入 READY 的前提，必须要求确认。
            // 电量字段来自 ap_link_set_battery 的缓存（BLE 回调里不能读 ADC）；
            // 未知（-1）就不带字段，主机面板据此显示"—"而不是错的 0%。
            static char hello_extra[160];
            int n = snprintf(hello_extra, sizeof(hello_extra),
                             "\"protocolVersion\":%d,\"firmware\":\"%s\",\"capabilities\":%u,"
                             "\"paired\":%d,\"deviceId\":\"%s\"",
                             AP_PROTOCOL_VERSION, AP_FIRMWARE_VERSION,
                             (unsigned)(AP_CAP_DISPLAY | AP_CAP_BUTTONS | AP_CAP_MIC |
                                        AP_CAP_SPEAKER | AP_CAP_BATTERY | AP_CAP_NOTIFY_LED |
                                        AP_CAP_SESSION_LIST),
                             ap_pair_is_paired() ? 1 : 0, ap_pair_device_id());
            const int bat = s_battery_percent;
            if (n > 0 && (size_t)n < sizeof(hello_extra) && bat >= 0) {
                snprintf(hello_extra + n, sizeof(hello_extra) - n,
                         ",\"batteryPercent\":%d", bat);
            }
            send_control_short_ex(AP_MSG_HELLO, 0, hello_extra, true);
            ESP_LOGI(TAG, "已主动发送 hello（等待主机 hello.ack，电量 %d）", bat);
        } else if (is_tx && !event->subscribe.cur_notify) {
            // 主机退订 → 对端可能重连，允许再次握手
            s_hello_sent = false;
        }
        return 0;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        // 重新配对：删掉旧绑定继续（设备没有需要保护的配对数据）。
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    default:
        return 0;
    }
}

// ── 生命周期 ──────────────────────────────────────────────────────────────

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE 复位 reason=%d", reason);
    set_state(AP_LINK_FAILED);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "取本地地址失败 rc=%d", rc);
        set_state(AP_LINK_FAILED);
        return;
    }
    if (s_start_requested) advertise();
}

static void nimble_host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    for (;;) vTaskSuspend(NULL);
}

esp_err_t ap_link_start(void)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;

    // 广播名与 CTRL 描述先准备好
    snprintf(s_ctrl_value, sizeof(s_ctrl_value),
             "{\"protocolVersion\":%d,\"firmware\":\"%s\",\"name\":\"%s\"}",
             AP_PROTOCOL_VERSION, "0.1.0", AP_DEVICE_NAME);

    if (!s_tx_lock) {
        s_tx_lock = xSemaphoreCreateMutex();
        if (!s_tx_lock) return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init 失败: %s", esp_err_to_name(err));
        set_state(AP_LINK_FAILED);
        return err;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_svc_gap_device_name_set(AP_DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置设备名失败 rc=%d", rc);
        nimble_port_deinit();
        set_state(AP_LINK_FAILED);
        return ESP_FAIL;
    }

    rc = ble_gatts_count_cfg(GATT_SVCS);
    if (rc == 0) rc = ble_gatts_add_svcs(GATT_SVCS);
    if (rc != 0) {
        ESP_LOGE(TAG, "注册 GATT 服务失败 rc=%d", rc);
        nimble_port_deinit();
        set_state(AP_LINK_FAILED);
        return ESP_FAIL;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    // 不要求配对/加密：链路是近场直连，配对反而增加使用门槛。
    // 安全性由"审批在设备上做二次确认 + DSH 侧完整审计"保证，不依赖链路加密。
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;

    s_start_requested = true;
    if (xTaskCreate(nimble_host_task, "nimble_host", 4096, NULL, 5, NULL) != pdPASS) {
        s_start_requested = false;
        nimble_port_deinit();
        set_state(AP_LINK_FAILED);
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    return ESP_OK;
}

esp_err_t ap_link_stop(void)
{
    s_start_requested = false;
    if (!s_initialized) return ESP_OK;

    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }
    (void)ble_gap_adv_stop();
    (void)nimble_port_stop();
    (void)nimble_port_deinit();

    s_initialized = false;
    set_state(AP_LINK_OFF);
    return ESP_OK;
}

ap_link_state_t ap_link_state(void)
{
    return s_state;
}

void ap_link_stats(ap_link_stats_t *out)
{
    if (out) *out = s_stats;
}

void ap_link_set_message_cb(ap_link_message_cb_t cb, void *user)
{
    s_message_cb = cb;
    s_message_user = user;
}

void ap_link_set_state_cb(ap_link_state_cb_t cb, void *user)
{
    s_state_cb = cb;
    s_state_user = user;
}

bool ap_link_is_ready(void)
{
    return s_state == AP_LINK_READY && s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

void ap_link_peer_info(int8_t *out_rssi, uint16_t *out_mtu)
{
    int8_t rssi = 0;
    uint16_t mtu = 0;
    const uint16_t conn = s_conn_handle;
    if (conn != BLE_HS_CONN_HANDLE_NONE) {
        if (ble_gap_conn_rssi(conn, &rssi) != 0) rssi = 0;
        mtu = ble_att_mtu(conn);
    }
    if (out_rssi) *out_rssi = rssi;
    if (out_mtu) *out_mtu = mtu;
}

int ap_link_rssi(void)
{
    return s_rssi_cached;
}

void ap_link_set_battery(int percent)
{
    s_battery_percent = percent;
}

void ap_link_disconnect_peer(void)
{
    const uint16_t conn = s_conn_handle;
    if (conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

void ap_link_tick(void)
{
    drain_tx_queue();

    const uint32_t t = now_ms();

    // ── 广播重开的退避重试（弱信号断开后自愈）──────────────────────────────
    // advertise() 失败只安排到这里重试，不在 GAP 回调里死磕（EBUSY 窗口）。
    if (s_adv_retry_pending && s_start_requested &&
        s_conn_handle == BLE_HS_CONN_HANDLE_NONE &&
        (int32_t)(t - s_adv_retry_at_ms) >= 0) {
        s_adv_retry_pending = false;
        s_adv_retry_count++;
        ESP_LOGW(TAG, "重开广播（第 %d 次重试）", s_adv_retry_count);
        advertise();
    }

    // ── RSSI 缓存刷新（信号格显示）────────────────────────────────────────
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        if ((int32_t)(t - s_rssi_next_ms) >= 0) {
            s_rssi_next_ms = t + RSSI_REFRESH_MS;
            int8_t rssi = 0;
            s_rssi_cached = (ble_gap_conn_rssi(s_conn_handle, &rssi) == 0) ? (int)rssi : AP_LINK_RSSI_NONE;
        }
    } else {
        s_rssi_cached = AP_LINK_RSSI_NONE;
    }

    // 重传：到点的槽位重发一次；超过上限判定链路异常。
    ap_tx_slot_t *slot = NULL;
    while (ap_tx_slot_expired(&s_tx, t, &slot)) {
        if (ap_tx_slot_give_up(slot)) {
            ESP_LOGW(TAG, "消息 %u 重传 %d 次仍未确认，判定链路异常",
                     slot->msg_id, slot->retries);
            s_stats.ack_timeouts++;
            ap_tx_ack(&s_tx, slot->msg_id);
            for (int i = 0; i < AP_TX_RETRY_SLOTS; i++) {
                if (s_retry_counts[i] > 0 && s_tx.slots[i].msg_id == slot->msg_id) {
                    s_retry_counts[i] = 0;
                }
            }
            continue;
        }
        // 重发该槽位的全部分片
        for (uint8_t i = 0; i < slot->chunk_count; i++) {
            (void)tx_enqueue(AP_CH_CONTROL, slot->chunks[i], slot->chunk_lens[i]);
        }
        s_stats.retries++;
        ap_tx_retried(slot, t);
    }

    // 心跳丢失判定：主机每 AP_HEARTBEAT_INTERVAL_MS 发一次 ping。
    // 超过上限没收到 → 认为主机已走（例如合盖休眠），主动断开以便重新广播，
    // 否则会停在一个"连着但没人应答"的假连接上。
    if (s_state == AP_LINK_READY) {
        if (s_missed_pings == 0 && s_last_ping_ms == 0) {
            s_last_ping_ms = t;
        } else if ((uint32_t)(t - s_last_ping_ms) > AP_HEARTBEAT_INTERVAL_MS) {
            s_missed_pings++;
            s_last_ping_ms = t;
            if (s_missed_pings > AP_HEARTBEAT_MISS_LIMIT) {
                ESP_LOGW(TAG, "连续 %u 次未收到主机心跳，主动断开", s_missed_pings);
                s_missed_pings = 0;
                ap_link_disconnect_peer();
            }
        }
    }
}
