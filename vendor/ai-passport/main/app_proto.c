// main/app_proto.c —— 协议的分片重组与 ACK 记账。
//
// ★ 这个文件刻意**不依赖 NimBLE、不依赖 LVGL、不依赖 FreeRTOS**：
//   纯数据结构 + 纯函数，因此可以在 host 上用 cc 直接编译并跑单元测试
//   （见 tests/test_app_proto.c）。分片边界、乱序、重复、缺片这些最容易写错、
//   又最难在真机上定位的逻辑，靠真机日志去查是折磨；放到 host 测试里是秒级反馈。
//
//   收发字节的活儿全部留给 app_link.c：这一层只回答"这堆字节拼出来是哪条消息"
//   和"哪几条消息还没被确认"。
#include "app_proto_logic.h"

#include <stdlib.h>
#include <string.h>

// ── 接收侧重组 ─────────────────────────────────────────────────────────────

void ap_rx_reset(ap_rx_t *rx)
{
    if (!rx) return;
    memset(rx, 0, sizeof(*rx));
}

// 把一条已完成的完整消息"交付"出去：拷贝到 msg 缓冲并置 ready。
// 之所以复制而不是让调用方直接用内部缓冲：调用方（app_link）需要在持有
// 该数据的同时去做 GATT 通知，内部缓冲随后可能被下一片覆盖。
static void ap_rx_deliver(ap_rx_t *rx)
{
    memcpy(rx->msg, rx->buf, rx->len);
    rx->msg_len = rx->len;
    rx->msg_id = rx->cur_id;
    rx->ready = true;
    // 交付后立刻清掉组装状态，允许同一 msgId 的对端重传被重新接收
    // （重传的处理交给上层：ready 未被取走前不再接收新消息，避免覆盖）。
    rx->assembling = false;
    rx->len = 0;
    rx->saw_last = false;
}

ap_rx_result_t ap_rx_push(ap_rx_t *rx, const uint8_t *chunk, uint16_t chunk_len)
{
    if (!rx || !chunk) return AP_RX_REJECT;
    if (chunk_len < AP_HEADER_BYTES) return AP_RX_REJECT;

    // 上一批交付还没被上层取走：丢弃新片而不是覆盖。
    // 覆盖会让上层拿到半新半旧的数据 —— 那比丢一条消息危险得多。
    if (rx->ready) return AP_RX_BUSY;

    const uint8_t version = ap_head_version(chunk);
    if (version != AP_PROTOCOL_VERSION) return AP_RX_VERSION_MISMATCH;

    const uint8_t flags = ap_head_flags(chunk);
    const uint16_t msg_id = ap_head_msg_id(chunk);
    const uint8_t seq = ap_head_seq(chunk);
    const uint8_t *payload = chunk + AP_HEADER_BYTES;
    const uint16_t payload_len = (uint16_t)(chunk_len - AP_HEADER_BYTES);

    if (flags & AP_FLAG_FIRST) {
        // FIRST 开启新消息。同 msgId 的残留（对端重传整条）直接替换：
        // 新的一轮重传才是权威内容。
        rx->assembling = true;
        rx->saw_last = false;
        rx->cur_id = msg_id;
        rx->len = 0;
        rx->next_seq = 0;
    } else if (!rx->assembling) {
        // 没有 FIRST 就来的中间片/尾片：可能我们中途才连上，也可能是丢片。
        // 都按 stale 处理，让上层决定是否要求对端重传。
        return AP_RX_STALE;
    } else if (msg_id != rx->cur_id) {
        // 不同消息的片交错到达：当前这条已经拼不完整了，整条丢弃。
        rx->assembling = false;
        rx->len = 0;
        return AP_RX_GAP;
    }

    if (seq != rx->next_seq) {
        // 重复片（seq 偏小）与缺片（seq 偏大）都在这里。
        rx->assembling = false;
        rx->len = 0;
        return AP_RX_GAP;
    }

    if ((uint32_t)rx->len + payload_len > AP_RX_BUFFER_BYTES) {
        rx->assembling = false;
        rx->len = 0;
        return AP_RX_OVERFLOW;
    }

    memcpy(rx->buf + rx->len, payload, payload_len);
    rx->len = (uint16_t)(rx->len + payload_len);
    rx->next_seq = (uint8_t)(seq + 1);
    if (flags & AP_FLAG_LAST) rx->saw_last = true;

    // 完成条件必须同时满足：见过 LAST、且片号从 0 连续（由上面的 next_seq 顺序检查保证）。
    if (rx->saw_last) {
        ap_rx_deliver(rx);
        return AP_RX_MESSAGE;
    }
    return AP_RX_INCOMPLETE;
}

const uint8_t *ap_rx_take(ap_rx_t *rx, uint16_t *out_len)
{
    if (!rx || !rx->ready) return NULL;
    if (out_len) *out_len = rx->msg_len;
    rx->ready = false;
    return rx->msg;
}

// ── 出站编码 ───────────────────────────────────────────────────────────────

uint16_t ap_encode(const uint8_t *payload, uint16_t payload_len, uint8_t channel,
                   uint16_t msg_id, bool ack_required, uint16_t chunk_payload,
                   ap_emit_fn emit, void *user)
{
    if (!payload || !emit) return 0;
    if (chunk_payload == 0) return 0;

    const uint16_t total = (uint16_t)((payload_len + chunk_payload - 1) / chunk_payload);
    if (total == 0 || total > AP_MAX_CHUNKS) return 0;

    uint8_t frame[AP_HEADER_BYTES + 512];
    if (chunk_payload > 512) return 0;

    uint16_t sent = 0;
    for (uint16_t seq = 0; seq < total; seq++) {
        const uint16_t offset = (uint16_t)(seq * chunk_payload);
        uint16_t slice = (uint16_t)(payload_len - offset);
        if (slice > chunk_payload) slice = chunk_payload;

        uint8_t flags = 0;
        if (seq == 0) flags |= AP_FLAG_FIRST;
        if (seq == total - 1) flags |= AP_FLAG_LAST;
        // ACK 语义挂在整条消息上，但每片都标：丢中间片时对端能立刻察觉并请求重传。
        if (ack_required) flags |= AP_FLAG_ACK_REQ;

        frame[0] = ap_head_byte0(AP_PROTOCOL_VERSION, flags);
        frame[1] = channel;
        frame[2] = (uint8_t)((msg_id >> 4) & 0xff);
        frame[3] = ap_head_byte3(msg_id, (uint8_t)seq);
        memcpy(frame + AP_HEADER_BYTES, payload + offset, slice);

        if (!emit(frame, (uint16_t)(AP_HEADER_BYTES + slice), user)) {
            // 发不出去就停：剩下的片继续发只会让对端收到半条消息并回 NACK。
            return 0;
        }
        sent++;
    }
    return sent;
}

// ── 出站重传记账 ───────────────────────────────────────────────────────────

void ap_tx_reset(ap_tx_t *tx)
{
    if (!tx) return;
    memset(tx, 0, sizeof(*tx));
}

bool ap_tx_track(ap_tx_t *tx, uint16_t msg_id, const uint8_t *const *chunks,
                 const uint16_t *chunk_lens, uint8_t chunk_count)
{
    if (!tx || !chunks || !chunk_lens) return false;
    if (chunk_count == 0 || chunk_count > AP_TX_RETRY_CHUNKS) return false;

    // 优先复用同 msgId 的槽（重传登记是幂等的），否则找空槽。
    int slot = -1;
    for (int i = 0; i < AP_TX_RETRY_SLOTS; i++) {
        if (tx->slots[i].in_use && tx->slots[i].msg_id == msg_id) { slot = i; break; }
    }
    if (slot < 0) {
        for (int i = 0; i < AP_TX_RETRY_SLOTS; i++) {
            if (!tx->slots[i].in_use) { slot = i; break; }
        }
    }
    // 槽满了：放弃登记。上层仍会把消息发出去，只是不重传 ——
    // 比覆盖掉别的待确认消息要安全（后者会让那条永远等不到 ACK）。
    if (slot < 0) return false;

    ap_tx_slot_t *s = &tx->slots[slot];
    s->in_use = true;
    s->msg_id = msg_id;
    s->retries = 0;
    s->chunk_count = chunk_count;
    for (uint8_t i = 0; i < chunk_count; i++) {
        s->chunks[i] = chunks[i];
        s->chunk_lens[i] = chunk_lens[i];
    }
    return true;
}

bool ap_tx_ack(ap_tx_t *tx, uint16_t msg_id)
{
    if (!tx) return false;
    for (int i = 0; i < AP_TX_RETRY_SLOTS; i++) {
        if (tx->slots[i].in_use && tx->slots[i].msg_id == msg_id) {
            tx->slots[i].in_use = false;
            return true;
        }
    }
    return false;
}

bool ap_tx_slot_expired(ap_tx_t *tx, uint32_t now_ms, ap_tx_slot_t **out_slot)
{
    if (!tx) return false;
    for (int i = 0; i < AP_TX_RETRY_SLOTS; i++) {
        ap_tx_slot_t *s = &tx->slots[i];
        if (!s->in_use) continue;
        // deadline 为 0 表示刚登记、还没设过超时点：立刻视为需要起算，不算过期。
        if (s->deadline_ms == 0) { s->deadline_ms = now_ms + AP_ACK_TIMEOUT_MS; continue; }
        if ((int32_t)(now_ms - s->deadline_ms) >= 0) {
            if (out_slot) *out_slot = s;
            return true;
        }
    }
    return false;
}

void ap_tx_retried(ap_tx_slot_t *slot, uint32_t now_ms)
{
    if (!slot) return;
    slot->retries++;
    // 固定间隔而不是指数退避：BLE 链路延迟稳定，固定间隔恢复更快。
    slot->deadline_ms = now_ms + AP_ACK_TIMEOUT_MS;
}

bool ap_tx_slot_give_up(const ap_tx_slot_t *slot)
{
    return slot && slot->retries >= AP_ACK_MAX_RETRIES;
}

uint8_t ap_tx_inflight(const ap_tx_t *tx)
{
    if (!tx) return 0;
    uint8_t n = 0;
    for (int i = 0; i < AP_TX_RETRY_SLOTS; i++) {
        if (tx->slots[i].in_use) n++;
    }
    return n;
}

// ── 极简 JSON 取值 ─────────────────────────────────────────────────────────
// 设备只从下行消息里取几个字段（type、若干字符串与数字）。为此引入完整 JSON
// 解析器会显著增加 Flash 占用与出错面，不值得。这里是**受控输入**下的取值器：
// 输入只可能来自我们自己的 host 插件，因此不追求覆盖任意合法 JSON。

static const char *ap_skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

// 定位 "key" 之后的冒号位置；找不到返回 NULL。
static const char *ap_find_key(const char *json, const char *key)
{
    if (!json || !key) return NULL;
    size_t key_len = strlen(key);
    const char *p = json;
    while ((p = strchr(p, '"')) != NULL) {
        p++;
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '"') {
            const char *colon = ap_skip_ws(p + key_len + 1);
            if (*colon == ':') return ap_skip_ws(colon + 1);
        }
    }
    return NULL;
}

bool ap_json_str(const char *json, const char *key, char *out, size_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    const char *p = ap_find_key(json, key);
    if (!p || *p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"') {
        char c = *p;
        if (c == '\\' && p[1]) {
            // 只处理 JSON 里最常见的三个转义；host 侧写入时已保证不会出现其它转义。
            p++;
            switch (*p) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            default: c = *p; break;
            }
        }
        // 为结尾 NUL 留位置：宁可截断也不越界。
        if (i + 1 >= out_size) return false;
        out[i++] = c;
        p++;
    }
    if (*p != '"') return false;
    out[i] = '\0';
    return true;
}

bool ap_json_num(const char *json, const char *key, long *out)
{
    const char *p = ap_find_key(json, key);
    if (!p) return false;
    // 只接受十进制整数（含负号）；小数直接判失败，避免悄悄丢精度。
    char *end = NULL;
    long value = strtol(p, &end, 10);
    if (end == p) return false;
    if (*end == '.' || *end == 'e' || *end == 'E') return false;
    if (out) *out = value;
    return true;
}

bool ap_json_bool(const char *json, const char *key, bool *out)
{
    const char *p = ap_find_key(json, key);
    if (!p) return false;
    if (strncmp(p, "true", 4) == 0) { if (out) *out = true; return true; }
    if (strncmp(p, "false", 5) == 0) { if (out) *out = false; return true; }
    return false;
}

// ── 扁平字段序列解码（协议 v2）─────────────────────────────────────────────
// 与 lib/protocol/flat.js 的转义规则逐条对应：
//   字段内 `|` 写作 `¦`（UTF-8: C2 A6），`\` 写作 `╲`（UTF-8: E2 95 B2）。
// 这里按**字节**识别转义，不引入完整 UTF-8 解码器（省 Flash，且本协议只用这两个字符）。

bool ap_flat_next(const char *src, size_t *pos, char *out, size_t out_size)
{
    if (!src || !pos || !out || out_size == 0) return false;
    size_t i = *pos;
    if (src[i] == '\0') {
        out[0] = '\0';
        return false;
    }
    size_t o = 0;
    while (src[i] != '\0' && src[i] != '|') {
        char ch;
        size_t advance;
        unsigned char b0 = (unsigned char)src[i];
        if (b0 == 0xC2 && (unsigned char)src[i + 1] == 0xA6) {
            ch = '|'; advance = 2;
        } else if (b0 == 0xE2 && (unsigned char)src[i + 1] == 0x95 &&
                   (unsigned char)src[i + 2] == 0xB2) {
            ch = '\\'; advance = 3;
        } else {
            ch = src[i]; advance = 1;
        }
        // 为 NUL 留位置：超长判失败（与 ap_json_str 的约定一致）
        if (o + 1 >= out_size) return false;
        out[o++] = ch;
        i += advance;
    }
    out[o] = '\0';
    if (src[i] == '|') i++; // 跳过分隔符；末尾的 '|' 之后视为结束
    *pos = i;
    return true;
}

size_t ap_flat_count(const char *src)
{
    if (!src || src[0] == '\0') return 0;
    size_t count = 1;
    for (size_t i = 0; src[i] != '\0'; i++) {
        if (src[i] == '|') count++;
    }
    return count;
}
