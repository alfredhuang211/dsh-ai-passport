// tests/test_app_proto.c —— AI Passport 端协议逻辑的 host 单元测试。
//
// 为什么这些用例值得单独存在：分片重组、乱序、重复片、ACK 重传这几件事
// **错了也能编译通过、也能跑起来**，只在真机上以"偶发"的形式表现出来。
// 靠真机日志查这类问题是折磨；放到 host 上跑是毫秒级。
//
// 运行方式见 tools/validate.sh 的静态门禁（用 cc 直接编译，不依赖 ESP-IDF）。
#include "app_proto_logic.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            g_failures++;                                                     \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                      \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

// ── 辅助：把一条消息编码进一个分片列表 ─────────────────────────────────────
#define MAX_TEST_CHUNKS 16
#define TEST_CHUNK_PAYLOAD 64

typedef struct {
    uint8_t frames[MAX_TEST_CHUNKS][AP_HEADER_BYTES + TEST_CHUNK_PAYLOAD];
    uint16_t lens[MAX_TEST_CHUNKS];
    uint8_t count;
} frame_list_t;

static frame_list_t g_frames;

static bool collect_frame(const uint8_t *frame, uint16_t frame_len, void *user)
{
    frame_list_t *list = (frame_list_t *)user;
    if (list->count >= MAX_TEST_CHUNKS) return false;
    memcpy(list->frames[list->count], frame, frame_len);
    list->lens[list->count] = frame_len;
    list->count++;
    return true;
}

static frame_list_t *encode(const char *payload, uint16_t chunk_payload, bool ack)
{
    memset(&g_frames, 0, sizeof(g_frames));
    uint16_t n = ap_encode((const uint8_t *)payload, (uint16_t)strlen(payload),
                           AP_CH_CONTROL, 7, ack, chunk_payload, collect_frame, &g_frames);
    return n == 0 ? NULL : &g_frames;
}

// ── 帧头 ───────────────────────────────────────────────────────────────────

static void test_header_roundtrip(void)
{
    printf("test_header_roundtrip\n");
    uint8_t chunk[AP_HEADER_BYTES] = {
        ap_head_byte0(AP_PROTOCOL_VERSION, AP_FLAG_FIRST | AP_FLAG_LAST | AP_FLAG_ACK_REQ),
        AP_CH_CONTROL,
        0xab,
        ap_head_byte3(0xabc, 5),
    };
    CHECK(ap_head_version(chunk) == AP_PROTOCOL_VERSION, "version");
    CHECK(ap_head_flags(chunk) == (AP_FLAG_FIRST | AP_FLAG_LAST | AP_FLAG_ACK_REQ), "flags");
    // 0xabc = 2748，只有 12 位有效：高 8 位 0xab，低 4 位 0xc
    CHECK(ap_head_msg_id(chunk) == 0xabc, "msgId 期望 0xabc 实际 0x%x", ap_head_msg_id(chunk));
    CHECK(ap_head_seq(chunk) == 5, "seq 期望 5 实际 %u", ap_head_seq(chunk));
}

static void test_msg_id_boundaries(void)
{
    printf("test_msg_id_boundaries\n");
    // 12 位边界必须能装下且不串位 —— 这是最容易写错的地方
    for (uint16_t id = 0; id <= 0xfff; id += 0x111) {
        // msgId 是 12 位：高 8 位在 byte2，低 4 位在 byte3 的高半字节。
        // 这里必须按协议构造，不能用 ap_head_byte3 一次塞完（它只负责低 4 位）。
        uint8_t chunk[AP_HEADER_BYTES] = {
            ap_head_byte0(1, 0), 0, (uint8_t)((id >> 4) & 0xff), ap_head_byte3(id, 0)
        };
        CHECK(ap_head_msg_id(chunk) == id, "msgId 往返失败：%u -> %u", id, ap_head_msg_id(chunk));
    }
    // 单测 0 与 4095 两个端点
    uint8_t lo[AP_HEADER_BYTES] = { ap_head_byte0(1, 0), 0, 0, ap_head_byte3(0, 0) };
    uint8_t hi[AP_HEADER_BYTES] = { ap_head_byte0(1, 0), 0, 0xff, ap_head_byte3(0xfff, 0) };
    CHECK(ap_head_msg_id(lo) == 0, "msgId 0");
    CHECK(ap_head_msg_id(hi) == 0xfff, "msgId 4095");
}

// ── 编码 ───────────────────────────────────────────────────────────────────

static void test_encode_single_chunk(void)
{
    printf("test_encode_single_chunk\n");
    frame_list_t *f = encode("{\"type\":\"ping\"}", TEST_CHUNK_PAYLOAD, false);
    CHECK(f != NULL && f->count == 1, "短消息应当单片");
    if (!f) return;
    CHECK(ap_head_flags(f->frames[0]) == (AP_FLAG_FIRST | AP_FLAG_LAST), "单片应同时带 FIRST/LAST");
    CHECK(ap_head_msg_id(f->frames[0]) == 7, "msgId");
    // 载荷必须逐字节一致
    uint16_t plen = (uint16_t)(f->lens[0] - AP_HEADER_BYTES);
    CHECK(plen == strlen("{\"type\":\"ping\"}"), "载荷长度");
    CHECK(memcmp(f->frames[0] + AP_HEADER_BYTES, "{\"type\":\"ping\"}", plen) == 0, "载荷内容");
}

static void test_encode_multi_chunk_flags(void)
{
    printf("test_encode_multi_chunk_flags\n");
    char payload[300];
    memset(payload, 'x', sizeof(payload) - 1);
    payload[sizeof(payload) - 1] = '\0';

    frame_list_t *f = encode(payload, TEST_CHUNK_PAYLOAD, true);
    CHECK(f != NULL, "应当编码成功");
    if (!f) return;
    CHECK(f->count == 5, "300 字节 / 64 = 5 片，实际 %u", f->count);
    CHECK(ap_head_flags(f->frames[0]) & AP_FLAG_FIRST, "首片带 FIRST");
    CHECK(!(ap_head_flags(f->frames[0]) & AP_FLAG_LAST), "首片不带 LAST");
    CHECK(!(ap_head_flags(f->frames[1]) & AP_FLAG_FIRST), "中间片不带 FIRST");
    CHECK(ap_head_flags(f->frames[f->count - 1]) & AP_FLAG_LAST, "尾片带 LAST");
    // 要求 ACK 时每片都要标：丢中间片时对端才能立刻察觉
    for (uint8_t i = 0; i < f->count; i++) {
        CHECK(ap_head_flags(f->frames[i]) & AP_FLAG_ACK_REQ, "第 %u 片应带 ACK_REQ", i);
        CHECK(ap_head_seq(f->frames[i]) == i, "第 %u 片 seq 应为 %u", i, i);
    }
}

static void test_encode_rejects_oversize(void)
{
    printf("test_encode_rejects_oversize\n");
    // 16 片 × 64 字节 = 1024；1025 字节必须被拒绝而不是静默截断
    static char big[1025];
    memset(big, 'y', sizeof(big));
    memset(&g_frames, 0, sizeof(g_frames));
    uint16_t n = ap_encode((const uint8_t *)big, sizeof(big), AP_CH_CONTROL, 1, false,
                           TEST_CHUNK_PAYLOAD, collect_frame, &g_frames);
    CHECK(n == 0, "超长载荷必须被拒绝，实际发出 %u 片", n);
    CHECK(g_frames.count == 0, "被拒绝时不应发出任何片");
}

// ── 重组 ───────────────────────────────────────────────────────────────────

static void test_reassemble_roundtrip(void)
{
    printf("test_reassemble_roundtrip\n");
    char payload[300];
    for (size_t i = 0; i < sizeof(payload) - 1; i++) payload[i] = (char)('a' + (i % 26));
    payload[sizeof(payload) - 1] = '\0';

    frame_list_t *f = encode(payload, TEST_CHUNK_PAYLOAD, false);
    CHECK(f != NULL, "编码成功");
    if (!f) return;

    ap_rx_t rx;
    ap_rx_reset(&rx);
    ap_rx_result_t last = AP_RX_INCOMPLETE;
    for (uint8_t i = 0; i < f->count; i++) {
        last = ap_rx_push(&rx, f->frames[i], f->lens[i]);
    }
    CHECK(last == AP_RX_MESSAGE || rx.ready, "最后一片应触发完成");
    uint16_t len = 0;
    const uint8_t *msg = ap_rx_take(&rx, &len);
    CHECK(msg != NULL, "应能取走消息");
    if (!msg) return;
    CHECK(len == strlen(payload), "长度一致：期望 %u 实际 %u", (unsigned)strlen(payload), len);
    CHECK(memcmp(msg, payload, len) == 0, "内容逐字节一致");
    CHECK(rx.ready == false, "取走后 ready 应清零");
}

static void test_reassemble_gap_discards(void)
{
    printf("test_reassemble_gap_discards\n");
    char payload[300];
    memset(payload, 'z', sizeof(payload) - 1);
    payload[sizeof(payload) - 1] = '\0';
    frame_list_t *f = encode(payload, TEST_CHUNK_PAYLOAD, false);
    if (!f) { CHECK(false, "编码失败"); return; }

    ap_rx_t rx;
    ap_rx_reset(&rx);
    CHECK(ap_rx_push(&rx, f->frames[0], f->lens[0]) == AP_RX_INCOMPLETE, "首片");
    // 跳过第 1 片直接喂第 2 片 → 缺口
    CHECK(ap_rx_push(&rx, f->frames[2], f->lens[2]) == AP_RX_GAP, "缺片应报 GAP");
    CHECK(rx.len == 0 && !rx.assembling, "缺口后必须清空组装状态，不能留半条");
    // 缺口之后的片没有归属
    CHECK(ap_rx_push(&rx, f->frames[3], f->lens[3]) == AP_RX_STALE, "缺口后的片应为 STALE");
}

static void test_reassemble_duplicate_is_idempotent(void)
{
    printf("test_reassemble_duplicate_is_idempotent\n");
    // 重传导致的重复片必须被忽略：否则消息会被拼重复、内容错位
    char payload[200];
    memset(payload, 'q', sizeof(payload) - 1);
    payload[sizeof(payload) - 1] = '\0';
    frame_list_t *f = encode(payload, TEST_CHUNK_PAYLOAD, true);
    if (!f) { CHECK(false, "编码失败"); return; }

    ap_rx_t rx;
    ap_rx_reset(&rx);
    // 首片重复送三次（模拟 ACK 丢失后的重传）
    for (int i = 0; i < 3; i++) {
        ap_rx_result_t r = ap_rx_push(&rx, f->frames[0], f->lens[0]);
        CHECK(r == AP_RX_INCOMPLETE || r == AP_RX_MESSAGE, "重复首片不应报错，实际 %d", r);
    }
    // 重复片会把 next_seq 保持在 1，后续片仍应正常拼接
    for (uint8_t i = 1; i < f->count; i++) {
        ap_rx_push(&rx, f->frames[i], f->lens[i]);
    }
    uint16_t len = 0;
    const uint8_t *msg = ap_rx_take(&rx, &len);
    CHECK(msg != NULL, "重复片之后仍应能拼出消息");
    if (msg) CHECK(len == strlen(payload) && memcmp(msg, payload, len) == 0, "内容未被重复片污染");
}

static void test_reassemble_first_without_last(void)
{
    printf("test_reassemble_first_without_last\n");
    // 只有 FIRST、没有 LAST 的单片：不能提前交付
    uint8_t chunk[AP_HEADER_BYTES + 4] = { ap_head_byte0(AP_PROTOCOL_VERSION, AP_FLAG_FIRST), AP_CH_CONTROL, 0, ap_head_byte3(1, 0), 'a', 'b', 'c', 'd' };
    ap_rx_t rx;
    ap_rx_reset(&rx);
    CHECK(ap_rx_push(&rx, chunk, sizeof(chunk)) == AP_RX_INCOMPLETE, "没有 LAST 不得交付");
    CHECK(!rx.ready, "ready 应为 false");
}

static void test_reassemble_busy_until_taken(void)
{
    printf("test_reassemble_busy_until_taken\n");
    // 上一条没被取走时，新消息必须被拒（BUSY），不能覆盖 ——
    // 覆盖会让上层拿到半新半旧的数据，比丢一条危险得多。
    frame_list_t *f = encode("{\"type\":\"toast\",\"text\":\"hi\"}", TEST_CHUNK_PAYLOAD, false);
    if (!f) { CHECK(false, "编码失败"); return; }
    ap_rx_t rx;
    ap_rx_reset(&rx);
    CHECK(ap_rx_push(&rx, f->frames[0], f->lens[0]) == AP_RX_MESSAGE, "首条应完成");
    CHECK(ap_rx_push(&rx, f->frames[0], f->lens[0]) == AP_RX_BUSY, "未取走时应 BUSY");
    ap_rx_take(&rx, NULL);
    CHECK(ap_rx_push(&rx, f->frames[0], f->lens[0]) == AP_RX_MESSAGE, "取走后应可再收");
}

static void test_reassemble_version_mismatch(void)
{
    printf("test_reassemble_version_mismatch\n");
    // 版本不一致必须显式报错：静默丢弃会让"连上了但什么都不动"变成谜案
    uint8_t chunk[AP_HEADER_BYTES] = { ap_head_byte0(9, AP_FLAG_FIRST | AP_FLAG_LAST), AP_CH_CONTROL, 0, ap_head_byte3(1, 0) };
    ap_rx_t rx;
    ap_rx_reset(&rx);
    CHECK(ap_rx_push(&rx, chunk, sizeof(chunk)) == AP_RX_VERSION_MISMATCH, "版本不符应显式报错");
}

static void test_reassemble_rejects_short_frame(void)
{
    printf("test_reassemble_rejects_short_frame\n");
    uint8_t tiny[2] = { 0, 0 };
    ap_rx_t rx;
    ap_rx_reset(&rx);
    CHECK(ap_rx_push(&rx, tiny, sizeof(tiny)) == AP_RX_REJECT, "不足帧头应拒绝");
}

static void test_reassemble_overflow(void)
{
    printf("test_reassemble_overflow\n");
    // 逐片灌到超过 AP_RX_BUFFER_BYTES：必须报 OVERFLOW 而不是越界写。
    // 注意片大小要选得让总量**超过**缓冲：若恰好等于（16×64 = 1024 = 缓冲），
    // 那是一条合法消息，会正常交付，测不到溢出路径。
    enum { BIG_PAYLOAD = 200 };
    static uint8_t frame[AP_HEADER_BYTES + BIG_PAYLOAD];
    ap_rx_t rx;
    ap_rx_reset(&rx);
    ap_rx_result_t r = AP_RX_INCOMPLETE;
    uint32_t total = 0;
    // 注意要正确设置 FIRST 与 LAST：完成条件要求"见过 LAST"，
    // 漏标 LAST 会让这条消息永远拼不完，于是用例永远等不到 OVERFLOW
    // —— 这是最初写这个用例时踩的坑，保留注释以免以后又漏。
    for (uint8_t seq = 0; seq < AP_MAX_CHUNKS && r == AP_RX_INCOMPLETE; seq++) {
        uint8_t flags = 0;
        if (seq == 0) flags |= AP_FLAG_FIRST;
        if (seq == AP_MAX_CHUNKS - 1) flags |= AP_FLAG_LAST;
        frame[0] = ap_head_byte0(AP_PROTOCOL_VERSION, flags);
        frame[1] = AP_CH_CONTROL;
        frame[2] = 0;
        frame[3] = ap_head_byte3(3, seq);
        memset(frame + AP_HEADER_BYTES, 'o', BIG_PAYLOAD);
        r = ap_rx_push(&rx, frame, sizeof(frame));
        total += BIG_PAYLOAD;
    }
    CHECK(r == AP_RX_OVERFLOW, "超过缓冲上限应报 OVERFLOW（累计 %u 字节），实际 %d", total, r);
    CHECK(rx.len == 0, "溢出后应清空");
}

// ── ACK 记账 ───────────────────────────────────────────────────────────────

static void test_tx_track_and_ack(void)
{
    printf("test_tx_track_and_ack\n");
    uint8_t buf[8] = { 0 };
    const uint8_t *chunks[1] = { buf };
    uint16_t lens[1] = { sizeof(buf) };
    ap_tx_t tx;
    ap_tx_reset(&tx);

    CHECK(ap_tx_track(&tx, 11, chunks, lens, 1), "登记应成功");
    CHECK(ap_tx_inflight(&tx) == 1, "在途应为 1");
    // 同 msgId 重复登记必须幂等（重传场景），不能占两个槽
    CHECK(ap_tx_track(&tx, 11, chunks, lens, 1), "重复登记应成功");
    CHECK(ap_tx_inflight(&tx) == 1, "重复登记后仍应为 1，实际 %u", ap_tx_inflight(&tx));

    CHECK(ap_tx_ack(&tx, 11), "ACK 应命中");
    CHECK(ap_tx_inflight(&tx) == 0, "确认后在途应归零");
    CHECK(!ap_tx_ack(&tx, 11), "重复 ACK 不应报命中");
    CHECK(!ap_tx_ack(&tx, 99), "未登记的 msgId 不应报命中");
}

static void test_tx_slots_exhaust(void)
{
    printf("test_tx_slots_exhaust\n");
    uint8_t buf[4] = { 0 };
    const uint8_t *chunks[1] = { buf };
    uint16_t lens[1] = { sizeof(buf) };
    ap_tx_t tx;
    ap_tx_reset(&tx);
    for (uint16_t id = 1; id <= AP_TX_RETRY_SLOTS; id++) {
        CHECK(ap_tx_track(&tx, id, chunks, lens, 1), "第 %u 条应登记成功", id);
    }
    // 槽满时必须**拒绝**而不是覆盖别人的槽 —— 覆盖会让那条永远等不到 ACK
    CHECK(!ap_tx_track(&tx, 100, chunks, lens, 1), "槽满应拒绝新登记");
    CHECK(ap_tx_inflight(&tx) == AP_TX_RETRY_SLOTS, "在途数应保持上限");
    // 确认一条后应能再登记
    CHECK(ap_tx_ack(&tx, 1), "确认第一条");
    CHECK(ap_tx_track(&tx, 100, chunks, lens, 1), "腾出槽后应可登记");
}

static void test_tx_expiry_and_give_up(void)
{
    printf("test_tx_expiry_and_give_up\n");
    uint8_t buf[4] = { 0 };
    const uint8_t *chunks[1] = { buf };
    uint16_t lens[1] = { sizeof(buf) };
    ap_tx_t tx;
    ap_tx_reset(&tx);
    ap_tx_track(&tx, 5, chunks, lens, 1);

    // 第一次调用只起算超时点，不算过期
    ap_tx_slot_t *slot = NULL;
    CHECK(!ap_tx_slot_expired(&tx, 1000, &slot), "刚登记不应立即过期");
    // 到点应过期
    CHECK(ap_tx_slot_expired(&tx, 1000 + AP_ACK_TIMEOUT_MS, &slot), "到点应过期");
    CHECK(slot != NULL && slot->msg_id == 5, "过期槽应是第 5 条");

    // 重传到上限后应放弃
    uint32_t now = 2000;
    for (int i = 0; i < AP_ACK_MAX_RETRIES; i++) {
        CHECK(!ap_tx_slot_give_up(slot), "第 %d 次重传前不应放弃", i);
        ap_tx_retried(slot, now);
        now += AP_ACK_TIMEOUT_MS;
    }
    CHECK(ap_tx_slot_give_up(slot), "达到重传上限后应放弃");
}

// ── JSON 取值 ──────────────────────────────────────────────────────────────

static void test_json_str(void)
{
    printf("test_json_str\n");
    char out[32];
    const char *json = "{\"type\":\"task.list\",\"title\":\"重构登录模块\",\"n\":3}";
    CHECK(ap_json_str(json, "type", out, sizeof(out)), "取 type 应成功");
    CHECK(strcmp(out, "task.list") == 0, "type 值：%s", out);
    CHECK(ap_json_str(json, "title", out, sizeof(out)), "取中文 title 应成功");
    CHECK(strcmp(out, "重构登录模块") == 0, "中文应逐字节保留：%s", out);
    CHECK(!ap_json_str(json, "missing", out, sizeof(out)), "不存在的 key 应失败");
    CHECK(!ap_json_str(json, "n", out, sizeof(out)), "数字字段用 str 取应失败");
}

static void test_json_str_rejects_overlong(void)
{
    printf("test_json_str_rejects_overlong\n");
    // 超长必须判失败而不是截断：截断后的字符串如果进 UI 会显示半截内容，
    // 更难查的是截断位置可能落在多字节 UTF-8 字符中间，直接显示成乱码。
    char out[8];
    const char *json = "{\"title\":\"这是一个很长很长的标题\"}";
    CHECK(!ap_json_str(json, "title", out, sizeof(out)), "超长应失败");
    // 刚好放得下（含结尾 NUL）应成功
    char exact[5];
    CHECK(ap_json_str("{\"t\":\"abcd\"}", "t", exact, sizeof(exact)), "刚好放得下应成功");
    CHECK(strcmp(exact, "abcd") == 0, "值：%s", exact);
}

static void test_json_num_and_bool(void)
{
    printf("test_json_num_and_bool\n");
    long n = 0;
    const char *json = "{\"batteryPercent\":87,\"neg\":-5,\"pi\":3.14,\"soundEnabled\":true,\"x\":false}";
    CHECK(ap_json_num(json, "batteryPercent", &n) && n == 87, "整数：%ld", n);
    CHECK(ap_json_num(json, "neg", &n) && n == -5, "负数：%ld", n);
    // 小数必须判失败：悄悄取整会让"3.14 秒"变成"3 秒"这种难以察觉的偏差
    CHECK(!ap_json_num(json, "pi", &n), "小数应失败");

    bool b = false;
    CHECK(ap_json_bool(json, "soundEnabled", &b) && b == true, "true");
    CHECK(ap_json_bool(json, "x", &b) && b == false, "false");
    CHECK(!ap_json_bool(json, "batteryPercent", &b), "数字不是布尔");
}

static void test_json_escapes(void)
{
    printf("test_json_escapes\n");
    char out[32];
    CHECK(ap_json_str("{\"t\":\"a\\nb\"}", "t", out, sizeof(out)), "转义应可解析");
    CHECK(strcmp(out, "a\nb") == 0, "换行转义：");
    CHECK(ap_json_str("{\"t\":\"a\\\"b\"}", "t", out, sizeof(out)), "引号转义应可解析");
    CHECK(strcmp(out, "a\"b") == 0, "引号转义");
}


static void test_flat_next_basic(void)
{
    printf("test_flat_next_basic\n");
    // 与 JS 侧 encodeSessionList 的输出逐字节对应
    const char *handles = "01|02|03";
    char out[8];
    size_t pos = 0;
    CHECK(ap_flat_next(handles, &pos, out, sizeof(out)), "字段 1");
    CHECK(strcmp(out, "01") == 0, "字段 1 = %s", out);
    CHECK(ap_flat_next(handles, &pos, out, sizeof(out)), "字段 2");
    CHECK(strcmp(out, "02") == 0, "字段 2 = %s", out);
    CHECK(ap_flat_next(handles, &pos, out, sizeof(out)), "字段 3");
    CHECK(strcmp(out, "03") == 0, "字段 3 = %s", out);
    CHECK(!ap_flat_next(handles, &pos, out, sizeof(out)), "迭代结束应返回 false");

    // 空串 = 0 个字段（主机侧空列表）
    pos = 0;
    CHECK(!ap_flat_next("", &pos, out, sizeof(out)), "空串无字段");
    CHECK(ap_flat_count("") == 0, "空串计数为 0");
    CHECK(ap_flat_count("a") == 1, "单字段计数");
    CHECK(ap_flat_count("a|") == 2, "尾分隔符计一个空字段");
    CHECK(ap_flat_count("a||b") == 3, "连续分隔符计空字段");
}

static void test_flat_next_unicode_escapes(void)
{
    printf("test_flat_next_unicode_escapes\n");
    // "a¦b" 里的 ¦ 是 U+00A6（C2 A6），应还原成竖线；"c╲d" 的 ╲ 应还原成反斜杠
    const char *titles = "a" "\xC2\xA6" "b|c" "\xE2\x95\xB2" "d";
    char out[32];
    size_t pos = 0;
    CHECK(ap_flat_next(titles, &pos, out, sizeof(out)), "字段 1");
    CHECK(strcmp(out, "a|b") == 0, "¦ 应还原为竖线：%s", out);
    CHECK(ap_flat_next(titles, &pos, out, sizeof(out)), "字段 2");
    CHECK(strcmp(out, "c\\d") == 0, "╲ 应还原为反斜杠：%s", out);
    CHECK(!ap_flat_next(titles, &pos, out, sizeof(out)), "应结束");

    // 多字节字符原样透传（UTF-8 中文不参与转义）
    const char *zh = "重构登录|修复乱码";
    pos = 0;
    CHECK(ap_flat_next(zh, &pos, out, sizeof(out)), "中文 1");
    CHECK(strcmp(out, "重构登录") == 0, "中文 1 = %s", out);

    // 输出缓冲不足时判失败，而不是产生半截字符串
    pos = 0;
    char small[3];
    CHECK(!ap_flat_next("abcdef", &pos, small, sizeof(small)), "超长应失败");
    pos = 0;
    CHECK(ap_flat_next("ab", &pos, small, sizeof(small)), "刚好放得下（含 NUL）应成功");
    CHECK(strcmp(small, "ab") == 0, "值 = %s", small);
}

// ── 入口 ───────────────────────────────────────────────────────────────────

int main(void)
{
    printf("== app_proto host tests ==\n");
    test_header_roundtrip();
    test_msg_id_boundaries();
    test_encode_single_chunk();
    test_encode_multi_chunk_flags();
    test_encode_rejects_oversize();
    test_reassemble_roundtrip();
    test_reassemble_gap_discards();
    test_reassemble_duplicate_is_idempotent();
    test_reassemble_first_without_last();
    test_reassemble_busy_until_taken();
    test_reassemble_version_mismatch();
    test_reassemble_rejects_short_frame();
    test_reassemble_overflow();
    test_tx_track_and_ack();
    test_tx_slots_exhaust();
    test_tx_expiry_and_give_up();
    test_json_str();
    test_json_str_rejects_overlong();
    test_json_num_and_bool();
    test_json_escapes();
    test_flat_next_basic();
    test_flat_next_unicode_escapes();

    if (g_failures == 0) {
        printf("== 全部通过 ==\n");
        return 0;
    }
    printf("== %d 项失败 ==\n", g_failures);
    return 1;
}
