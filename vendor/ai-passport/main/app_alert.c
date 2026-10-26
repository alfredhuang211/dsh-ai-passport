// main/app_alert.c —— 提示音实现（采样咳嗽 + BP 机音型，运行时合成/回放）
//
// 两代方案的取舍（docs/09 §6.8）：
//   · 音型类（完成/出错/中断/确认）→ **运行时合成**：零 Flash、参数可调、
//     3200Hz/140ms 这类 BP 机节奏用正弦段拼出来最准；
//   · 咳嗽类（待审批/追问）→ **采样回放**：合成咳嗽怎么调都不像人咳，
//     用户明确要求用 QQ 的咳嗽采样。采样体积 10.4KB + 4.7KB，可接受。
//
// ★ 版权：采样头文件（alert_cough_*.h）由 tools/make-sound-asset.mjs 从本地
//   素材生成，**不进仓库**（.gitignore）。缺失时自动回落到合成音，保证
//   CI/未下载素材的构建仍可编译、可用。
//
// ★ 与语音录音互斥：录音期间绝不播音——两者共用 I2S/ES8311，提示音会改变
//   采样率与通道格式，插进录音流里会直接毁掉这段语音。录音时静默跳过即可。
#include "app_alert.h"

#include <math.h>
#include <string.h>

#include "app_voice.h"
#include "bsp_audio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#if __has_include("alert_cough_double.h")
#include "alert_cough_double.h"
#define HAVE_COUGH_DOUBLE 1
#endif
#if __has_include("alert_cough_single.h")
#include "alert_cough_single.h"
#define HAVE_COUGH_SINGLE 1
#endif

static const char *TAG = "app_alert";

// 音型用 16k（BSP 默认格式）；咳嗽采样是 8k（QQ 素材原始采样率，省一半 Flash）。
#define TONE_RATE 16000
#define COUGH_RATE 8000
#define TONE_MS_MAX 400                  // 单段音调最长时长
#define FADE_MS 5                        // 淡入淡出，消除咔哒声
#define ALERT_VOLUME_DEFAULT 70          // 中档；运行期由设置页/主机改
#define ALERT_QUEUE_DEPTH 4              // 极短队列：允许"完成 + 出错"这类并发
#define ALERT_DEDUPE_MS 400              // 同一音型短时间重复到达只响一次

typedef struct {
    uint16_t freq_hz;   // 0 = 静音（用于在连响之间插间隔）
    uint16_t ms;
} tone_t;

// ── 音型定义 ──────────────────────────────────────────────────────────────

// 完成：BP 机四连哔（3200Hz，140ms 响 / 110ms 停）
static const tone_t TONES_BP_COMPLETED[] = {
    { 3200, 140 }, { 0, 110 },
    { 3200, 140 }, { 0, 110 },
    { 3200, 140 }, { 0, 110 },
    { 3200, 140 },
};

// 出错：同样四连哔，末尾接一段 600Hz 低尾音 —— "完成"与"出错"必须一耳可分
static const tone_t TONES_BP_ERROR[] = {
    { 3200, 140 }, { 0, 110 },
    { 3200, 140 }, { 0, 110 },
    { 3200, 140 }, { 0, 110 },
    { 3200, 140 }, { 0, 80 },
    { 600, 200 },
};

// 中断：三连哔（比完成短一截，不混淆）
static const tone_t TONES_BP_ABORTED[] = {
    { 3200, 140 }, { 0, 110 },
    { 3200, 140 }, { 0, 110 },
    { 3200, 140 },
};

// 确认（审批已选择）：上行双音
static const tone_t TONES_DECIDED[] = {
    { 1200, 70 }, { 0, 30 }, { 1500, 90 },
};

static const tone_t TONES_PAIR_OK[] = {
    { 1200, 70 }, { 0, 25 }, { 1800, 120 },
};

static const tone_t TONES_PAIR_FAIL[] = {
    { 900, 110 }, { 0, 40 }, { 600, 180 },
};

static const tone_t TONES_LOW_BATTERY[] = {
    { 600, 160 }, { 0, 70 }, { 600, 160 },
};

// 无采样时的合成兜底：短促低音双击/单击（不像咳嗽，但能与其它提示音区分）
static const tone_t TONES_APPROVAL_FALLBACK[] = {
    { 420, 70 }, { 0, 45 }, { 360, 90 },
};
static const tone_t TONES_QUESTION_FALLBACK[] = {
    { 420, 80 },
};

// ── 状态 ──────────────────────────────────────────────────────────────────

static bool s_initialized = false;
static uint8_t s_volume = ALERT_VOLUME_DEFAULT;
static bool s_enabled = true;
static QueueHandle_t s_queue = NULL;
static TaskHandle_t s_task = NULL;
static int16_t s_buf[TONE_RATE * TONE_MS_MAX / 1000];
static alert_kind_t s_last_kind = ALERT_NONE;
static uint32_t s_last_ms = 0;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/** 合成一段正弦音调进 s_buf，返回样本数。 */
static size_t synth_tone(uint16_t freq_hz, uint16_t ms)
{
    const size_t n = (size_t)TONE_RATE * ms / 1000;
    const size_t fade = (size_t)TONE_RATE * FADE_MS / 1000;
    const float two_pi_f = 2.0f * (float)M_PI * (float)freq_hz / (float)TONE_RATE;

    for (size_t i = 0; i < n && i < sizeof(s_buf) / sizeof(s_buf[0]); i++) {
        if (freq_hz == 0) {
            s_buf[i] = 0;                       // 间隔：静音
            continue;
        }
        float v = sinf(two_pi_f * (float)i);
        if (i < fade) v *= (float)i / (float)fade;
        else if (i + fade > n) v *= (float)(n - i) / (float)fade;
        s_buf[i] = (int16_t)(v * 12000.0f);
    }
    return n;
}

/** 播放一串音调。返回 false 表示音频通道不可用。 */
static bool play_tones(const tone_t *tones, size_t count)
{
    if (bsp_audio_set_format(TONE_RATE, 16, 1) != ESP_OK) {
        ESP_LOGW(TAG, "设置音频格式失败，跳过提示音");
        return false;
    }
    bsp_audio_set_volume(s_volume);

    for (size_t i = 0; i < count; i++) {
        const size_t n = synth_tone(tones[i].freq_hz, tones[i].ms);
        if (n == 0) continue;
        if (bsp_audio_write(s_buf, n * sizeof(int16_t)) != ESP_OK) {
            ESP_LOGW(TAG, "音频写入失败，提示音中断");
            return false;
        }
    }
    return true;
}

/** 咳嗽采样回放；单咳用 4.7KB 独立采样（与双咳不是前缀关系，必须分开存）。 */
static bool play_cough(bool single)
{
#if defined(HAVE_COUGH_DOUBLE) || defined(HAVE_COUGH_SINGLE)
    const int16_t *pcm = NULL;
    size_t count = 0;
    if (single) {
#if defined(HAVE_COUGH_SINGLE)
        pcm = ALERT_COUGH_SINGLE_PCM;
        count = ALERT_COUGH_SINGLE_SAMPLE_COUNT;
#elif defined(HAVE_COUGH_DOUBLE)
        // 只有双咳采样时，截前 1/3 当单咳（约 0.22s，仍像一声咳）
        pcm = ALERT_COUGH_DOUBLE_PCM;
        count = ALERT_COUGH_DOUBLE_SAMPLE_COUNT / 3;
#endif
    } else {
#if defined(HAVE_COUGH_DOUBLE)
        pcm = ALERT_COUGH_DOUBLE_PCM;
        count = ALERT_COUGH_DOUBLE_SAMPLE_COUNT;
#endif
    }
    if (pcm && count > 0) {
        if (bsp_audio_set_format(COUGH_RATE, 16, 1) == ESP_OK) {
            bsp_audio_set_volume(s_volume);
            if (bsp_audio_write(pcm, count * sizeof(int16_t)) == ESP_OK) return true;
            ESP_LOGW(TAG, "咳嗽采样写入失败，回落到合成音");
        }
    }
#endif
    return play_tones(single ? TONES_QUESTION_FALLBACK : TONES_APPROVAL_FALLBACK,
                      single ? sizeof(TONES_QUESTION_FALLBACK) / sizeof(TONES_QUESTION_FALLBACK[0])
                             : sizeof(TONES_APPROVAL_FALLBACK) / sizeof(TONES_APPROVAL_FALLBACK[0]));
}

static void play_kind(alert_kind_t kind)
{
    // 录音期间不播音：I2S/ES8311 格式会被改掉，正在录的语音直接损坏。
    if (app_voice_recording()) {
        ESP_LOGD(TAG, "录音中，跳过提示音 kind=%d", (int)kind);
        return;
    }
    if (kind == s_last_kind && (uint32_t)(now_ms() - s_last_ms) < ALERT_DEDUPE_MS) {
        ESP_LOGD(TAG, "提示音 %d 刚响过，去重跳过", (int)kind);
        return;
    }
    s_last_kind = kind;
    s_last_ms = now_ms();

    switch (kind) {
    case ALERT_APPROVAL:
        (void)play_cough(false);
        break;
    case ALERT_QUESTION:
        (void)play_cough(true);
        break;
    case ALERT_COMPLETED:
        (void)play_tones(TONES_BP_COMPLETED,
                         sizeof(TONES_BP_COMPLETED) / sizeof(TONES_BP_COMPLETED[0]));
        break;
    case ALERT_ERROR:
        (void)play_tones(TONES_BP_ERROR, sizeof(TONES_BP_ERROR) / sizeof(TONES_BP_ERROR[0]));
        break;
    case ALERT_ABORTED:
        (void)play_tones(TONES_BP_ABORTED,
                         sizeof(TONES_BP_ABORTED) / sizeof(TONES_BP_ABORTED[0]));
        break;
    case ALERT_DECIDED:
        (void)play_tones(TONES_DECIDED, sizeof(TONES_DECIDED) / sizeof(TONES_DECIDED[0]));
        break;
    case ALERT_PAIR_OK:
        (void)play_tones(TONES_PAIR_OK, sizeof(TONES_PAIR_OK) / sizeof(TONES_PAIR_OK[0]));
        break;
    case ALERT_PAIR_FAIL:
        (void)play_tones(TONES_PAIR_FAIL, sizeof(TONES_PAIR_FAIL) / sizeof(TONES_PAIR_FAIL[0]));
        break;
    case ALERT_LOW_BATTERY:
        (void)play_tones(TONES_LOW_BATTERY,
                         sizeof(TONES_LOW_BATTERY) / sizeof(TONES_LOW_BATTERY[0]));
        break;
    default:
        break;
    }
}

static void alert_task(void *arg)
{
    (void)arg;
    alert_kind_t kind;
    for (;;) {
        if (xQueueReceive(s_queue, &kind, portMAX_DELAY) == pdTRUE) {
            play_kind(kind);
        }
    }
}

esp_err_t alert_init(void)
{
    if (s_initialized) return ESP_OK;

    s_queue = xQueueCreate(ALERT_QUEUE_DEPTH, sizeof(alert_kind_t));
    if (!s_queue) return ESP_ERR_NO_MEM;

    if (xTaskCreate(alert_task, "app_alert", 4096, NULL, 4, &s_task) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "提示音就绪（咳嗽采样%s + BP 音型）",
#if defined(HAVE_COUGH_DOUBLE) && defined(HAVE_COUGH_SINGLE)
             " 10.4KB/4.7KB"
#elif defined(HAVE_COUGH_DOUBLE) || defined(HAVE_COUGH_SINGLE)
             "（单/双之一）"
#else
             "缺失→合成兜底"
#endif
    );
    return ESP_OK;
}

void alert_play(alert_kind_t kind, bool enabled)
{
    if (!s_initialized || !enabled || kind == ALERT_NONE) return;

    // 审批/追问优先级最高：清掉积压的完成/确认音，避免"先响旧提示再响审批"。
    if (kind == ALERT_APPROVAL || kind == ALERT_QUESTION) {
        alert_kind_t dropped;
        while (xQueueReceive(s_queue, &dropped, 0) == pdTRUE) { /* 丢弃旧请求 */ }
        (void)xQueueSend(s_queue, &kind, 0);
        return;
    }

    // 其它音型：队列满时覆盖最老的一条（新状态比旧的更值得播报）。不阻塞调用方。
    if (xQueueSend(s_queue, &kind, 0) != pdTRUE) {
        alert_kind_t dropped;
        (void)xQueueReceive(s_queue, &dropped, 0);
        (void)xQueueSend(s_queue, &kind, 0);
    }
}

bool alert_kind_from_name(const char *name, alert_kind_t *out)
{
    if (!name || !out) return false;
    struct { const char *name; alert_kind_t kind; } table[] = {
        { "approve",     ALERT_APPROVAL },
        { "question",    ALERT_QUESTION },
        { "completed",   ALERT_COMPLETED },
        { "error",       ALERT_ERROR },
        { "aborted",     ALERT_ABORTED },
        { "decided",     ALERT_DECIDED },
        { "pair_ok",     ALERT_PAIR_OK },
        { "pair_fail",   ALERT_PAIR_FAIL },
        { "low_battery", ALERT_LOW_BATTERY },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcmp(name, table[i].name) == 0) {
            *out = table[i].kind;
            return true;
        }
    }
    return false;
}

uint8_t alert_get_volume(void) { return s_volume; }

void alert_set_volume(uint8_t percent)
{
    const uint8_t next = percent > 100 ? 100 : percent;
    if (next != s_volume) {
        ESP_LOGI(TAG, "提示音量 %u%% → %u%%", (unsigned)s_volume, (unsigned)next);
    }
    s_volume = next;
    s_enabled = next > 0;
}

bool alert_get_setting(void) { return s_volume > 0; }

void alert_set_setting(bool enabled)
{
    alert_set_volume(enabled ? ALERT_VOLUME_DEFAULT : 0);
}
