// main/app_voice.c —— 设备侧录音上行实现。
//
// 流水线（每 20ms 一轮，全在独立任务里）：
//   bsp_audio_read(20ms PCM) → IMA-ADPCM 编码 → 累积进发送缓冲
//   → 缓冲够一条消息(< AP_MAX_PAYLOAD_BYTES)就 ap_link_send_voice 发出
//
// ★ 三个不能踩的坑（详见 docs/05-语音发送-设计.md）：
//   1. **绝不缓冲整段录音**：400KB RAM 装不下 30s（32KB/s 原始 + BLE 栈 + LVGL）。
//      用静态缓冲、够一条消息就发、源端丢帧。
//   2. **ADPCM 表必须与 voice.js 的 decodeImaAdpcm 逐字节一致**，否则解出来是噪声。
//      两张表从 voice.js 抄，附注释，不"优化"。
//   3. **一次录音跨多条 BLE 消息**（单消息 2048B 载荷上限），主机按到达顺序拼
//      （voice.js 已改为流式拼接）。设备侧复用同一 sessionId 分多条发。
#include "app_voice.h"

#include <math.h>
#include <string.h>

#include "app_link.h"
#include "app_proto.h"
#include "app_ui.h"
#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "app_voice";

// ── 常量 ──────────────────────────────────────────────────────────────────
#define SAMPLE_RATE        16000
#define BITS               16
#define CHANNELS           1
#define READ_MS            20                    // 每次读 20ms = 320 样本 = 640B
#define READ_SAMPLES       (SAMPLE_RATE * READ_MS / 1000)   // 320
#define READ_BYTES         (READ_SAMPLES * 2)    // 640

// 录音上限：直接用协议常量，**不要**在这里另写一份（见 app_proto.h 的说明）。
#define MAX_MS             (AP_AUDIO_MAX_SECONDS * 1000)  // 录音上限，与协议契约同源
#define MAX_READS          (MAX_MS / READ_MS)              // 30s → 1500 轮

// 发送缓冲：ADPCM 4:1 后 20ms ≈ 160B。攒到 ~1600B（< 2048 载荷上限）就发一条。
// 留余量，避免 tx_enqueue 因超上限拒绝。
#define FLUSH_THRESHOLD    1600
#define SEND_BUF_SIZE      2048

// ── IMA/DVI ADPCM 表（与 voice.js decodeImaAdpcm 逐字节一致）───────────────
// 步长表：解码器 stepIndex 钳在 0..88，故只用前 89 项（7 … 691）。
// 从 packages/dsh-ai-passport/lib/bridge/voice.js 的 IMA_STEP_TABLE 抄来。
static const int16_t IMA_STEP_TABLE[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21,
    23, 25, 28, 31, 34, 37, 40, 43, 48, 52, 56, 60,
    64, 68, 73, 78, 83, 89, 94, 99, 105, 112, 117, 124,
    130, 137, 144, 151, 157, 163, 170, 177, 184, 191, 198, 205,
    212, 219, 226, 233, 240, 247, 255, 262, 270, 278, 286, 294,
    303, 312, 321, 331, 341, 351, 362, 373, 384, 396, 408, 420,
    433, 446, 459, 473, 487, 502, 517, 532, 548, 564, 581, 598,
    616, 634, 653, 672, 691,
};

// 索引步进表：nibble → stepIndex 增量。与 voice.js IMA_INDEX_TABLE 一致。
static const int8_t IMA_INDEX_TABLE[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8,
};

// ── ADPCM 编码器状态（一条录音一份；跨块保持 predictor/stepIndex）──────────
typedef struct {
    int32_t predictor;   // 上一个样本（编码端用 32 位避免溢出）
    int32_t step_index;  // 0..88
} adpcm_state_t;

/** 把一个 16bit 样本编码成 4bit nibble，返回 0..15。编码端与解码端算法对偶。 */
static int adpcm_encode_nibble(adpcm_state_t *s, int16_t sample)
{
    int step = IMA_STEP_TABLE[s->step_index];
    int diff = sample - s->predictor;
    int nibble = 0;
    if (diff < 0) { nibble = 8; diff = -diff; }
    if (diff >= step) { nibble |= 4; diff -= step; }
    if (diff >= (step >> 1)) { nibble |= 2; diff -= (step >> 1); }
    if (diff >= (step >> 2)) { nibble |= 1; diff -= (step >> 2); }

    // 重建预测值（与解码端同式，保证编码/解码一致）
    int delta = step >> 3;
    if (nibble & 1) delta += step >> 2;
    if (nibble & 2) delta += step >> 1;
    if (nibble & 4) delta += step;
    if (nibble & 8) s->predictor -= delta;
    else           s->predictor += delta;
    if (s->predictor > 32767) s->predictor = 32767;
    else if (s->predictor < -32768) s->predictor = -32768;

    s->step_index += IMA_INDEX_TABLE[nibble];
    if (s->step_index < 0) s->step_index = 0;
    else if (s->step_index > 88) s->step_index = 88;
    return nibble;
}

// ── 状态 ──────────────────────────────────────────────────────────────────
typedef enum { VOICE_IDLE = 0, VOICE_RECORDING, VOICE_FLUSHING } voice_state_t;

static volatile voice_state_t s_state = VOICE_IDLE;
// 松手请求（可能早于任务醒来到达）。stop() 置位、任务唤醒时消费、start() 清除。
static volatile bool s_stop_pending = false;
// 录音意图：start() 置位、stop() 清除。用于覆盖"任务还没醒"的窗口
// （此时 s_state 仍是 VOICE_IDLE，但用户已经按下、录音已开始意图）。
static volatile bool s_recording_intent = false;
static TaskHandle_t s_task = NULL;
static SemaphoreHandle_t s_lock = NULL;   // 保护 s_state/缓冲的短临界区

static bool s_initialized = false;
static uint16_t s_session_id = 0;
// 协议 v2：目标会话 handle（2 位十六进制）。写入只发生在按键/LVGL 任务，
// 读取在录音任务起始处快照一次，因此不需要锁（最坏情况是本次录音用旧目标）。
static char s_target[AP_SESSION_HANDLE_LEN + 1];

// 发送缓冲（静态，不走堆）
static uint8_t s_send_buf[SEND_BUF_SIZE];
static size_t s_send_len = 0;

// ADPCM 块状态与"本块已用数据字节"（每 504B 数据字节开一个新块头）
static adpcm_state_t s_adpcm;
static int s_block_bytes = 0;            // 当前 ADPCM 块已写的数据字节数
#define ADPCM_BLOCK_DATA 504              // 与主机解码的 504B 数据边界一致

static int s_read_count = 0;             // 本轮已读次数（MAX_MS 上限）

static int16_t s_pcm[READ_SAMPLES];      // 20ms PCM 读缓冲

// ── 工具 ──────────────────────────────────────────────────────────────────

/** 计算 20ms PCM 的 RMS（仅用于电平上报排障，不参与自动结束判定）。 */
static int32_t rms_of(const int16_t *pcm, int n)
{
    int64_t sum = 0;
    for (int i = 0; i < n; i++) sum += (int32_t)pcm[i] * pcm[i];
    return (int32_t)sqrt((double)sum / n);
}

/** 往发送缓冲写一个 ADPCM 块头（4 字节）：predictor + stepIndex + 保留。 */
static void write_block_header(void)
{
    s_send_buf[s_send_len++] = (uint8_t)(s_adpcm.predictor & 0xff);
    s_send_buf[s_send_len++] = (uint8_t)((s_adpcm.predictor >> 8) & 0xff);
    s_send_buf[s_send_len++] = (uint8_t)s_adpcm.step_index;
    s_send_buf[s_send_len++] = 0;   // 保留字节
    s_block_bytes = 0;
}

/** 缓冲够阈值就发一条消息（清空缓冲）。 */
static void flush_send_buf(void)
{
    if (s_send_len == 0) return;
    esp_err_t rc = ap_link_send_voice(s_session_id, s_send_buf, (uint16_t)s_send_len);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "语音分片发送失败 rc=%d（丢这一批，继续下一批）", (int)rc);
    }
    s_send_len = 0;
    // ★ 这里**不**重置 s_block_bytes、不补块头：
    //   主机把多条 BLE 消息按到达顺序拼成连续字节流再解码（assembleWav 先 concat），
    //   半个块被拆到两条消息里会无缝重接。只有真正写满 504 数据字节才开新块头。
}

/**
 * 编码 20ms PCM → ADPCM 进缓冲，够阈值就发送。
 *
 * 缓冲布局：[块头4B][数据504B][块头4B][数据504B]…
 * 每写一个数据字节前，若 s_block_bytes==0 先补块头；满 504 数据字节则下个字节前再补。
 */
static void encode_and_flush(void)
{
    for (int i = 0; i < READ_SAMPLES; i += 2) {
        // 需要 4 字节余量（1 数据字节 + 最坏情况的 4B 块头）
        if (s_send_len + 4 >= SEND_BUF_SIZE) {
            flush_send_buf();
            if (s_send_len + 4 >= SEND_BUF_SIZE) return;   // 仍放不下，丢这一轮
        }
        // 块边界：新块的第一个字节前补块头
        if (s_block_bytes == 0 || s_block_bytes >= ADPCM_BLOCK_DATA) {
            write_block_header();
        }

        // 一个字节 = 2 个样本 = 2 个 nibble（低 nibble 先）
        int n0 = adpcm_encode_nibble(&s_adpcm, s_pcm[i]);
        int n1 = (i + 1 < READ_SAMPLES) ? adpcm_encode_nibble(&s_adpcm, s_pcm[i + 1]) : 0;
        s_send_buf[s_send_len++] = (uint8_t)((n1 << 4) | (n0 & 0x0f));
        s_block_bytes++;

        if (s_send_len >= FLUSH_THRESHOLD) flush_send_buf();
    }
}

// ── 采集任务 ──────────────────────────────────────────────────────────────

static void app_voice_task(void *arg)
{
    (void)arg;
    for (;;) {
        // 等开始信号（阻塞，不空转）
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // 快速点按保护：松手可能早于任务醒来（此时 s_state 还是 IDLE，
        // stop() 只会置 s_stop_pending）—— 若不检查，录音会无视松手一路录到
        // VAD 超时。按下→任务醒来通常只差几毫秒，但这个窗口必须堵住。
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_stop_pending) {
            s_stop_pending = false;
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG, "快速松手，放弃本次录音");
            continue;
        }
        xSemaphoreGive(s_lock);

        // 进入录音：设格式、复位状态、发 voice.begin
        if (bsp_audio_set_format(SAMPLE_RATE, BITS, CHANNELS) != ESP_OK) {
            ESP_LOGE(TAG, "设置音频格式失败，本次录音取消");
            app_ui_post_toast("录音失败：音频未就绪", true);
            continue;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        // set_format 期间可能又松手了，再查一次
        if (s_stop_pending) {
            s_stop_pending = false;
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG, "set_format 期间松手，放弃本次录音");
            continue;
        }
        s_state = VOICE_RECORDING;
        s_session_id = (uint16_t)((s_session_id + 1) & 0x0f);   // 4bit 内循环
        app_ui_set_recording(true);   // 通知 UI：录音开始，缓存期间忽略 task.state
        s_send_len = 0;
        s_block_bytes = 0;
        s_adpcm.predictor = 0;
        s_adpcm.step_index = 0;
        s_read_count = 0;
        xSemaphoreGive(s_lock);

        char target_field[24] = "";
        if (s_target[0]) {
            snprintf(target_field, sizeof(target_field), "\"target\":\"%s\",", s_target);
        }
        char begin[160];
        snprintf(begin, sizeof(begin),
                 "{\"type\":\"" AP_MSG_VOICE_BEGIN "\",\"sessionId\":%u,%s"
                 "\"sampleRate\":%d,\"bits\":%d,\"channels\":%d,\"codec\":\"ima-adpcm\"}",
                 (unsigned)s_session_id, target_field, SAMPLE_RATE, BITS, CHANNELS);
        // ★ 不要求 ACK（false）：voice.begin 是通知性消息，同 voice.end 的说明。
        (void)ap_link_send_json(begin, false);
        // 粘性提示：按住说话期间一直显示，松手后由收尾处隐藏
        app_ui_post_toast_sticky("开始说话", false);
        ESP_LOGI(TAG, "开始录音 session=%u", (unsigned)s_session_id);

        // ── 采集循环：每 20ms 读一块 → VAD → 编码 ──
        //
        // ★ bsp_audio_read 失败 ≠ 识别失败。它只表示"这一轮没读到样本"（流末尾 /
        //   短暂 IO 抖动），而此刻发送缓冲里可能已经有大半段可识别的音频。
        //
        //   修复前这里直接 app_ui_post_toast("音频读取失败…", true) 弹一条**紧急**
        //   toast（4 秒），把随后 2.5 秒才轮到的"识别中…/识别结果"toast 盖住 ——
        //   用户看到的顺序变成"先失败、后出字"，实际上主机早已正常识别出文字。
        //
        //   现在改为：读不到样本就**安静地收尾**，把已编码的音频照常发 voice.end，
        //   让主机去识别；只有"整段一个有效样本都没采到"才提示真正的录音失败
        //   （见下方收尾处的 s_read_count==0 分支）。
        bool stop = false;
        int32_t max_rms = 0;   // 本段录音的原始 PCM 峰值 RMS（编码前电平，随 voice.end 上报）
        while (!stop) {
            if (s_state != VOICE_RECORDING) break;   // 被 stop() 叫停

            if (bsp_audio_read(s_pcm, READ_BYTES) != ESP_OK) {
                // 不 toast、不报"识别失败"：这轮没读到样本，就此收尾送识别。
                ESP_LOGI(TAG, "音频本轮无样本（已录 %d 轮），收尾送识别", s_read_count);
                break;
            }
            s_read_count++;

            // 电平统计：仅用于随 voice.end 上报峰值 RMS，供主机侧排障（识别结果为空时，
            // 用来区分"根本没采到声音"和"采到了但没识别出来"）。
            //
            // ★ 这里**不再做任何静音/自动结束判定**。录音结束只有两个来源：
            //   松手（s_stop_pending）或达到 MAX_MS 上限。中间的停顿、思考、
            //   句间留白都原样录进去，由识别模型自己处理上下文。
            int32_t rms = rms_of(s_pcm, READ_SAMPLES);
            if (rms > max_rms) max_rms = rms;

            encode_and_flush();

            // MAX_MS 上限（唯一的自动结束条件之一，另一是松手）
            if (s_read_count >= MAX_READS) {
                ESP_LOGI(TAG, "达到 %dms 上限，自动结束录音", MAX_MS);
                stop = true;
            }
        }

        // 收尾：发剩余缓冲 + voice.end（带原始电平，供主机侧排障：
        // maxRms≈0 → 麦克风没采到；很大 → 采集本身就过载/异常）
        flush_send_buf();
        char end[96];
        snprintf(end, sizeof(end),
                 "{\"type\":\"" AP_MSG_VOICE_END "\",\"sessionId\":%u,\"durationMs\":%d,\"maxRms\":%d}",
                 (unsigned)s_session_id, s_read_count * READ_MS, (int)max_rms);
        // ★ 不要求 ACK（false）：voice.end 是通知性消息，丢了靠主机侧超时兜底。
        //   要求 ACK 会在松手后的流量切换点（音频停→voice.end→主机 transcribe）
        //   触发重传，加剧 BLE 拥塞；且重传会让主机重复处理 voice.end，
        //   表现为"插件显示断开/闪过识别失败"（真机返工记录）。
        (void)ap_link_send_json(end, false);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_state = VOICE_IDLE;
        xSemaphoreGive(s_lock);
        app_ui_set_recording(false);  // 通知 UI：录音结束，应用录音期间缓存的 task.state

        // ★ 松手后的提示策略（真机返工记录）：
        //   - "开始说话"是粘性提示，这里先隐藏它（说话已结束）；
        //   - **不显示"识别中…"**（用户明确不需要）；
        //   - 只有"整段一个有效样本都没采到"才提示录音失败；
        //   - 识别结果/识别失败由主机回的 voice.result / voice.error 决定，
        //     用户要求"识别失败可以提示"，成功结果也照常显示。
        app_ui_hide_toast();
        if (s_read_count == 0) {
            app_ui_post_toast("录音失败：没采到声音", true);
            ESP_LOGW(TAG, "本次录音一个样本都没采到（session=%u）", (unsigned)s_session_id);
        }
        ESP_LOGI(TAG, "录音结束 session=%u 时长=%dms",
                 (unsigned)s_session_id, s_read_count * READ_MS);
    }
}

// ── 对外接口 ──────────────────────────────────────────────────────────────

esp_err_t app_voice_init(void)
{
    if (s_initialized) return ESP_OK;

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    // 栈：含 bsp_audio_read（阻塞 IO）+ sqrt + snprintf。给 4096 余量。
    if (xTaskCreate(app_voice_task, "app_voice", 4096, NULL, 5, &s_task) != pdPASS) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "语音上行就绪（PTT 按住说话，上限 %dms）", MAX_MS);
    return ESP_OK;
}

bool app_voice_start(void)
{
    if (!s_initialized) return false;
    if (s_state != VOICE_IDLE) return false;   // 已在录音/收尾，忽略

    // 新的按住覆盖上一次的松手标记，再通知任务
    s_stop_pending = false;
    s_recording_intent = true;   // 录音意图：覆盖"任务还没醒"的窗口
    // 只发通知，采集在任务里做（按键任务不能碰音频）
    xTaskNotifyGive(s_task);
    return true;
}

void app_voice_stop(void)
{
    // 始终置松手标记：即使任务还没醒来（state 仍 IDLE），也能让它醒后放弃。
    s_stop_pending = true;
    s_recording_intent = false;   // 按键松开：录音意图消失
    // 让采集循环在下一轮退出（任务里检查 state）
    if (s_state == VOICE_RECORDING) s_state = VOICE_FLUSHING;
}

static bool target_char_ok(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static char target_lower(char c)
{
    return (c >= 'A' && c <= 'F') ? (char)(c - 'A' + 'a') : c;
}

void app_voice_set_target(const char *handle)
{
    char next[AP_SESSION_HANDLE_LEN + 1] = { 0 };
    if (handle && handle[0]) {
        // 只接受 1~2 位十六进制：协议层不信任界面传值，非法值直接当"未指定"。
        if (target_char_ok(handle[0]) && (handle[1] == '\0' || target_char_ok(handle[1]))) {
            next[0] = target_lower(handle[0]);
            if (handle[1]) next[1] = target_lower(handle[1]);
        }
    }
    s_target[0] = next[0];
    s_target[1] = next[1];
    s_target[2] = '\0';
}

const char *app_voice_target(void) { return s_target; }

bool app_voice_recording(void)
{
    // ★ 启动窗口也算"录音中"：app_voice_start() 只发通知，任务醒来到设
    //   s_state = VOICE_RECORDING 之间有几十毫秒窗口。若此时 task.state 推送
    //   到达，旧的 s_state != VOICE_IDLE 判定为 false，守卫失效。
    //   用 s_stop_pending 的语义反转：start() 清零它，stop() 置位它 ——
    //   因此"已发 start 但还没到 stop"（s_stop_pending == false 且任务已通知）
    //   就是录音意图存在。这里用一个独立的 s_recording_intent 标志更直观。
    return s_state != VOICE_IDLE || s_recording_intent;
}

void app_voice_on_result(const char *text)
{
    if (text && text[0]) app_ui_post_toast(text, false);
}

void app_voice_on_error(const char *err)
{
    app_ui_post_toast((err && err[0]) ? err : "语音识别失败", true);
}
