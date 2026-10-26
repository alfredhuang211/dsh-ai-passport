// main/app.c —— 应用入口：把 BLE 链路、界面与按键串起来。
//
// ★ 上游规范（AGENTS.md）要求二次开发"重新设计并实现独立 UI，禁止沿用当前 demo
//   测试菜单"。因此这个应用**不经菜单**，上电直接进入联机状态界面。
//   demo_*.c 仍留在树里（它们是 BSP 的可运行参考，也是回归基线），
//   只是不再从本应用可达 —— 需要它们时改回调用 demo 菜单即可。
//
// 线程模型（三个执行体，边界必须清楚）：
//   · NimBLE host 任务：BLE 回调。只做解析与投递，绝不碰 LVGL、绝不阻塞。
//   · app 任务（本文件创建）：每 100ms 调 ap_link_tick()；处理按键事件；
//     处理从 BLE 回调入队的应用消息。
//   · LVGL 任务：界面渲染。只有它和持有 bsp_lvgl_lock() 的代码能碰 LVGL 对象。
//
// 为什么消息要再入队一次（BLE 回调 → 队列 → app 任务）：
//   BLE 回调里同时做"解析 + 更新界面 + 发响应"会让回调长时间占用协议栈任务，
//   在链路拥塞时表现为"连接还在但什么都不响应"。回调只入队，是最省心的边界。
#include "app_link.h"
#include "app_proto.h"
#include "app_proto_logic.h"
#if CONFIG_APP_ENABLE_SCREENSHOT
#include "app_screenshot.h"
#endif
#include "app_screens.h"
#include "app_session.h"
#include "app_alert.h"
#include "app_pair.h"
#include "app_settings.h"
#include "app_voice.h"
#include "app_ui.h"

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_attr.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "app_main";

// 应用消息队列：BLE 回调入队，app 任务消费。
#define APP_MSG_QUEUE_DEPTH 8
// 一条下行 JSON 的最大长度。与 app_link 的接收缓冲对齐。
#define APP_MSG_MAX_LEN 1024

typedef struct {
    char json[APP_MSG_MAX_LEN];
} app_msg_t;

static QueueHandle_t s_msg_queue;
static TaskHandle_t s_app_task;

// 按键事件（与 bsp_button.h 的枚举对齐，但本文件自己转换，便于独立测试）。
typedef struct {
    uint8_t btn;
    uint8_t event;
} key_msg_t;
static QueueHandle_t s_key_queue;

static void post_key(uint8_t btn, uint8_t event);

static volatile bool s_running;
static int s_battery_percent = -1;

// 当前任务状态（由主机推送，设备只显示不推导）。
// 记录它是为了"只在状态**变化**时响提示音" —— 主机可能周期性重推同一状态，
// 每次都响会把设备变成噪音源，用户很快就会把提示音关掉。
static ap_task_state_t s_task_state = AP_TASK_STATE_IDLE;

// 协议 v2：会话列表/详情模型（由 session.* 消息维护，界面只读）。
static ap_session_list_t s_sessions;
static ap_session_detail_t s_session_detail;
// 显示配置（主机 config 下发；设备不持久化，避免两边各存一份分叉）。
static bool s_cfg_mask_titles = false;
static bool s_cfg_detail_summary = true;
static bool s_cfg_detail_elapsed = true;
// 最近一次"用户在动"的时刻（按键 / 收到主机消息 / 覆盖页交互）。
// 自动休眠只看这个时间戳，配合 ap_settings_should_power_off 的纯判定。
static uint32_t s_last_activity_ms = 0;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ★ 深睡期间保留的魔数（RTC 内存）：用来区分"冷启动"和"从我们自己的深睡里醒来"。
//   冷启动（拔电/换电池）时 RTC 内存丢失 → 魔数不在 → 正常开机，
//   这样"休眠标记"不会把真正的断电重启也挡住。
#define POWER_OFF_MAGIC 0x4f46465aU   // "OFFZ"
RTC_DATA_ATTR static uint32_t s_power_off_magic = 0;
// 连续"非按键唤醒"计数（RTC 保留）：用来判断是不是陷入了"睡→被误唤醒→再睡"的循环
RTC_DATA_ATTR static uint32_t s_spurious_wake_count = 0;

/**
 * 唤醒瞬间"按键是不是真按着"。
 *
 * 判据来自 BSP 的分压读数：松开约 3300mV，三个键按下分别是 0~1900mV。
 * 真按键唤醒时用户的手还按在键上（否则唤不醒），所以开机瞬间读到的应该是"按下"；
 * 读到"松开"就说明是**误唤醒**（引脚电平抖动、USB/JTAG、欠压等）。
 *
 * 留一个窗口反复读，是为了容忍"按下后很快松手"的快按。
 */
#define BTN_PRESSED_MAX_MV 2500    // 低于此值算按下（松开约 3300）
#define BTN_PRESSED_WINDOW_MS 600  // 读这么久：容忍"按一下就松手"的快按
static bool button_is_pressed(int window_ms)
{
    const int step_ms = 50;
    int last_mv = 0;
    for (int waited = 0; waited <= window_ms; waited += step_ms) {
        const int mv = bsp_button_read_mv();
        last_mv = mv;
        if (mv <= 0) {
            // 读不到（ADC 还没就绪等）→ **当作按下**：宁可多开一次机，
            // 也不能让用户按了没反应（"按不亮"比"偶尔多醒一次"糟得多）。
            ESP_LOGW(TAG, "唤醒时读不到按键分压（%d），按已按下处理", mv);
            return true;
        }
        if (mv < BTN_PRESSED_MAX_MV) {
            ESP_LOGI(TAG, "唤醒时读到按键按下（%d mV）", mv);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(step_ms));
    }
    ESP_LOGW(TAG, "唤醒后 %dms 内按键始终松开（最后一次 %d mV）", window_ms, last_mv);
    return false;
}

/**
 * 深睡前把背光"按住"低电平。
 *
 * ★ 真机反馈"休眠后它自己又亮起来"：背光由 LEDC PWM 驱动，深睡时 LEDC 掉电、
 *   GPIO21 变成悬空输入，背光就被点亮了（屏幕发白/发亮，看起来像自己开机）。
 *   这里先把引脚拉低并启用 pad hold，深睡期间保持低电平。
 */
static void backlight_hold_off(void)
{
    bsp_display_backlight(0);
    gpio_set_level((gpio_num_t)BSP_LCD_BL, 0);
    gpio_hold_en((gpio_num_t)BSP_LCD_BL);
    gpio_deep_sleep_hold_en();
}

/** 开机时释放背光引脚的 hold（否则醒来后屏幕一直是黑的）。 */
static void backlight_hold_release(void)
{
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis((gpio_num_t)BSP_LCD_BL);
}

/**
 * 休眠前把按键引脚从 ADC 交回 RTC IO，并打开上拉。
 *
 * ★ 真机踩到的坑（拔掉 USB 后仍"低频闪 + 自己重启"）：GPIO0 平时挂在 **ADC 模式**，
 *   数字输入通路是关的；深睡的低电平唤醒于是把这种状态当成"一直按下"，
 *   周期性把自己唤醒（每次唤醒都会亮一下背光，就是那阵低频闪）。
 *   交回 RTC IO + 上拉后，松开的节点是稳定的高电平，只有真按下才拉低。
 */
static void wake_pin_prepare(void)
{
    const gpio_num_t pin = (gpio_num_t)BSP_BTN_ADC_GPIO;
    // C3 的 RTC IO **不支持**方向/上下拉控制（SOC_RTCIO_INPUT_OUTPUT_SUPPORTED=0），
    // 所以走数字 IO：把引脚从 ADC 交回普通 GPIO 并开数字上拉，让深睡唤醒看到真实电平。
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_pullup_en(pin);
    gpio_pulldown_dis(pin);
    vTaskDelay(pdMS_TO_TICKS(5));   // 等上拉把节点拉稳
    ESP_LOGI(TAG, "唤醒引脚 GPIO%d 已交回普通 GPIO+上拉，电平=%d（1=高，松开应为 1）",
             BSP_BTN_ADC_GPIO, gpio_get_level(pin));
}

/** 只配唤醒源就睡回去：用于"误唤醒时保持休眠"，此时外设都还没初始化。 */
static void stay_off(void) __attribute__((noreturn));
static void stay_off(void)
{
    wake_pin_prepare();
    const esp_err_t err = esp_deep_sleep_enable_gpio_wakeup(1ULL << BSP_BTN_ADC_GPIO,
                                                            ESP_GPIO_WAKEUP_GPIO_LOW);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "按键唤醒配置失败：%s", esp_err_to_name(err));
    }
    s_power_off_magic = POWER_OFF_MAGIC;
    backlight_hold_off();          // 休眠期间背光必须是暗的（见函数注释）
    esp_deep_sleep_start();
    ESP_LOGE(TAG, "保持休眠的深睡意外返回，重启");
    esp_restart();
    while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}

/**
 * 开机早期：这次启动该不该真的开机？
 *
 * 规则（真机反馈"设了自动休眠，到点又自己重启了"）：
 *   · RTC 魔数在  → 是从我们自己的深睡里醒来的：
 *       按键唤醒（GPIO/EXT1）→ 开机（清标记）；
 *       其它唤醒源（USB/JTAG、欠压、看门狗…）→ **重新睡回去**，保持休眠。
 *   · 魔数不在    → 冷启动（真断电）→ 正常开机。
 */
static void auto_sleep_gate(void)
{
    // 上次不是自动休眠 → 正常开机（顺便清掉可能残留的标记）
    if (!ap_settings_is_powered_off()) return;

    const esp_reset_reason_t reason = esp_reset_reason();
    // 真上电/外部复位（拔插电源、按复位键）→ 让它开机，别把人关在外面
    if (reason == ESP_RST_POWERON || reason == ESP_RST_EXT || reason == ESP_RST_UNKNOWN) {
        ESP_LOGI(TAG, "上电复位（reason=%d）：正常开机", (int)reason);
        (void)ap_settings_set_powered_off(false);
        return;
    }

    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    const bool button_cause = (cause == ESP_SLEEP_WAKEUP_GPIO || cause == ESP_SLEEP_WAKEUP_EXT1 ||
                               cause == ESP_SLEEP_WAKEUP_EXT0);

    // ★ 判据一：必须是**深睡唤醒**（ESP_RST_DEEPSLEEP）。
    //   ★ 真机踩到的坑：插着 USB 时，主机侧串口句柄/DTR 变化会触发
    //     rst:0x15 (USB_UART_CHIP_RESET) —— 那是**芯片复位**，不是用户按键，
    //     于是"睡下去 1 秒又被拉起来"，看起来就是"没休眠、自己重启了"。
    //     这类复位（USB/看门狗/软件/panic）一律睡回去。
    // ★ 判据二：唤醒源是按键，且开机瞬间按键**确实按着**（引脚抖动也会报 GPIO）。
    if (reason == ESP_RST_DEEPSLEEP && button_cause &&
        (button_is_pressed(BTN_PRESSED_WINDOW_MS) || s_spurious_wake_count >= 2)) {
        ESP_LOGI(TAG, "按键唤醒（reason=%d cause=%d）：正常开机（此前连续误唤醒 %u 次）",
                 (int)reason, (int)cause, (unsigned)s_spurious_wake_count);
        s_power_off_magic = 0;
        s_spurious_wake_count = 0;
        (void)ap_settings_set_powered_off(false);
        return;
    }

    s_spurious_wake_count++;
    ESP_LOGW(TAG, "误唤醒（reason=%d cause=%d 按键源=%d，第 %u 次）—— 保持休眠，继续深睡",
             (int)reason, (int)cause, (int)button_cause, (unsigned)s_spurious_wake_count);
    stay_off();
}

// ── BLE 回调（跑在 NimBLE 任务里，只入队）──────────────────────────────────

static void on_link_message(const char *json, void *user)
{
    (void)user;
    if (!s_msg_queue || !json) return;
    app_msg_t msg;
    snprintf(msg.json, sizeof(msg.json), "%s", json);
    // 队列满就丢最老的？不 —— 丢弃并且记一条日志。
    // 丢最老的会让"完成提醒"这类消息被后续的无关消息挤掉，用户会漏掉提醒。
    if (xQueueSend(s_msg_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "应用消息队列已满，丢弃一条");
    }
}

static void on_link_state(ap_link_state_t state, void *user)
{
    (void)user;
    // 这个回调也在 NimBLE 任务里，因此只投递状态，界面刷新交给 LVGL 定时器。
    app_ui_on_link_state(state);
    ESP_LOGI(TAG, "链路状态 -> %d", state);
}

// 审批请求：{type, id, toolName, reason, displayReason, risk, timeoutMs}
static void handle_approval_req(const char *json)
{
    char id[40] = { 0 };
    char summary[64] = { 0 };
    char session_title[AP_SESSION_TITLE_MAX] = { 0 };
    char risk[8] = { 0 };
    long timeout = 0;
    ap_json_str(json, "id", id, sizeof(id));
    // ★ 设备只收一行极短摘要（完整工具名/原因在电脑卡片上，用户决策 2026-10-06）：
    //   summary 是新字段；回退 toolName 兼容旧主机。
    if (!ap_json_str(json, "summary", summary, sizeof(summary))) {
        ap_json_str(json, "toolName", summary, sizeof(summary));
    }
    // 协议 v2：审批属于哪个会话（旧主机无此字段 → 空串，审批页整行隐藏）。
    ap_json_str(json, "title", session_title, sizeof(session_title));
    ap_json_str(json, "risk", risk, sizeof(risk));
    const bool has_timeout = ap_json_num(json, "timeoutMs", &timeout);

    ESP_LOGI(TAG, "收到审批请求：%s（%s）", summary, id);
    // ★ 提示音跟**审批请求**走，不跟 task.state 变化走。之前只在状态切到
    //   「待审批」时响，但状态推送可能被去重（状态没变）、被录音守卫吞掉，
    //   或者干脆没推 —— 表现为"弹窗了但不响"甚至"什么都没有"（真机返工记录）。
    alert_play(ALERT_APPROVAL, alert_get_setting());
    app_screens_post_approval(id, summary, session_title, strcmp(risk, "high") == 0,
                              has_timeout && timeout > 0 ? (uint32_t)timeout : 30000);
}

// 审批结束：{type, id, outcome, decidedBy} —— 用来收起页面（例如电脑端先答了）
static void handle_approval_result(const char *json)
{
    char id[40] = { 0 };
    char outcome[24] = { 0 };
    ap_json_str(json, "id", id, sizeof(id));
    ap_json_str(json, "outcome", outcome, sizeof(outcome));
    ESP_LOGI(TAG, "审批已结束：%s -> %s", id, outcome);
    app_screens_post_approval_done(id, outcome);

    if (strcmp(outcome, "allowed-once") == 0) {
        app_ui_post_toast("电脑端已允许", false);
    } else if (strcmp(outcome, "rejected") == 0) {
        app_ui_post_toast("电脑端已拒绝", false);
    } else if (strcmp(outcome, "timeout") == 0) {
        app_ui_post_toast("审批已超时", true);
    }
}

// 余额：{type, currency, totalBalance, rechargeBalance, bonusBalance, todayUsed, ...}
// 注意：金额是 JSON 数字（可能是小数），而设备的 ap_json_num 故意不接小数。
// 因此在设备上做一次极简的"取到小数点后两位"的字符串转换，而不是引入浮点解析。
static void format_money(const char *json, const char *key, char *out, size_t out_size)
{
    out[0] = '\0';
    // 先按字符串找一次（主机也可能直接给格式化好的文本）
    if (ap_json_str(json, key, out, out_size)) return;

    // 否则按数字扫描：定位 "key": 之后，拷贝连续的数字与小点，最多两位小数。
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return;
    p = strchr(p + strlen(pattern), ':');
    if (!p) return;
    p++;
    while (*p == ' ') p++;

    size_t i = 0;
    int decimals = -1;
    if (*p == '-') { if (i + 1 < out_size) out[i++] = *p; p++; }
    while ((*p >= '0' && *p <= '9') || *p == '.') {
        if (*p == '.') {
            if (decimals >= 0) break;
            decimals = 0;
        } else if (decimals >= 0) {
            decimals++;
            if (decimals > 2) break;   // 只保留两位，不做四舍五入（余额展示不需要）
        }
        if (i + 1 >= out_size) break;
        out[i++] = *p;
        p++;
    }
    out[i] = '\0';
}

static void handle_balance(const char *json)
{
    char currency[8] = { 0 };
    char total[24] = { 0 };
    char recharge[24] = { 0 };
    char bonus[24] = { 0 };
    char today[24] = { 0 };
    ap_json_str(json, "currency", currency, sizeof(currency));
    format_money(json, "totalBalance", total, sizeof(total));
    format_money(json, "rechargeBalance", recharge, sizeof(recharge));
    format_money(json, "bonusBalance", bonus, sizeof(bonus));
    format_money(json, "todayUsed", today, sizeof(today));

    ESP_LOGI(TAG, "余额 %s %s（充值 %s / 赠送 %s）", currency, total, recharge, bonus);
    app_screens_post_balance(total, currency, recharge, bonus, today);
}

static void handle_toast(const char *json)
{
    char text[80] = { 0 };
    bool urgent = false;
    ap_json_str(json, "text", text, sizeof(text));
    ap_json_bool(json, "urgent", &urgent);
    if (text[0]) app_ui_post_toast(text, urgent);
}

// ── 任务状态（精简版的核心下行消息）──────────────────────────────────────

/**
 * 处理主机下发的任务状态：{state, title?}
 *
 * ★ 状态由**主机聚合**后下发，设备不做任何判断。理由是状态推导需要知道
 *   "有几个会话在跑""有没有审批挂起""刚才是否结束"，这些只有主机侧才知道。
 *   设备端猜出来的状态一定会与主机不一致，而一个会撒谎的状态指示器比没有更糟。
 *
 * ★ 状态变化时响提示音。只对"需要用户注意"的两个状态响
 *   （待审批 / 已完成），运行中与空闲保持安静 —— 否则设备会一直在响，
 *   用户很快会把提示音当噪音并关掉，那时真正的审批提醒也会被忽略。
 */
static void handle_task_state(const char *json)
{
    // ★ 录音期间**直接忽略** task.state 推送。
    //
    //   主机侧已做"录音期间抑制推送"，但链路重连/重新握手会绕过它
    //   （onLinkReady 会清掉录音标志），于是"已完成→空闲"的回落定时器
    //   在用户还没松手时就把状态打回空闲 —— 表现为"按住说话到一半界面
    //   突然跳到空闲，但录音还在继续"（真机返工记录）。
    //
    //   设备端是最终防线：只要 app_voice_recording() 为真，就丢弃这条推送。
    //   录音结束后主机补推的那条会正常生效（此时 recording 已为假）。
    if (app_voice_recording()) {
        ESP_LOGD(TAG, "录音中，忽略 task.state 推送");
        return;
    }

    char state[24] = { 0 };
    char title[64] = { 0 };
    ap_json_str(json, "state", state, sizeof(state));
    ap_json_str(json, "title", title, sizeof(title));

    ap_task_state_t st = AP_TASK_STATE_IDLE;
    if (strcmp(state, "running") == 0)               st = AP_TASK_STATE_RUNNING;
    else if (strcmp(state, "waiting_approval") == 0) st = AP_TASK_STATE_WAITING_APPROVAL;
    else if (strcmp(state, "completed") == 0)        st = AP_TASK_STATE_COMPLETED;

    const bool changed = (st != s_task_state);
    s_task_state = st;
    // 用户要求：有状态变化时优先回主页显示运行状态 —— 设置页是"我在翻设置"，
    // 不该压住"任务开始/完成/等待审批"这类变化。（审批/追问走各自覆盖页，优先级更高。）
    if (app_screens_current() == AP_SCREEN_SETTINGS) {
        app_screens_hide(AP_SCREEN_SETTINGS);
        ESP_LOGI(TAG, "任务状态变化，自动收起系统设置页");
    }
    app_ui_set_task_state(st, title);

    if (changed) {
        ESP_LOGI(TAG, "任务状态 -> %s%s%s", state,
                 title[0] ? "（" : "", title[0] ? title : "");
    }

    // 只在**状态真的变化**时响，避免主机重复推送同一状态导致反复响铃
    if (changed) {
        // ★ 审批提示音改在 handle_approval_req 里响（跟请求走，不跟状态走），
        //   这里只保留"任务完成"提示音，避免同一个审批响两次。
        if (st == AP_TASK_STATE_COMPLETED) {
            alert_play(ALERT_COMPLETED, alert_get_setting());
        }
    }
}

/**
 * 处理主机下发的设备配置：{soundEnabled?}
 *
 * ★ 提示音开关放在主机侧配置，而不是设备上的持久设置：
 *   设备端写 NVS 会带来磨损，而且两边各存一份必然分叉 ——
 *   用户在面板上关了提示音，设备却还记得旧的"开"，这种不一致极难排查。
 *   设备只保存内存态，每次连上都由主机下发，唯一事实来源在主机。
 */
static void handle_config(const char *json)
{
    bool sound = true;
    if (ap_json_bool(json, "soundEnabled", &sound)) {
        alert_set_setting(sound);
    }
    // 协议 v2 显示配置：标题打码 / 详情次要信息开关（主机为唯一事实源）。
    bool mask = s_cfg_mask_titles;
    bool summary = s_cfg_detail_summary;
    bool elapsed = s_cfg_detail_elapsed;
    if (ap_json_bool(json, "maskTitles", &mask)) s_cfg_mask_titles = mask;
    if (ap_json_bool(json, "detailSummary", &summary)) s_cfg_detail_summary = summary;
    if (ap_json_bool(json, "detailElapsed", &elapsed)) s_cfg_detail_elapsed = elapsed;
}

// ── 多会话模型（协议 v2）──────────────────────────────────────────────────

const ap_session_list_t *app_session_list(void) { return &s_sessions; }
const ap_session_detail_t *app_session_detail(void) { return &s_session_detail; }
bool app_session_mask_titles(void) { return s_cfg_mask_titles; }
bool app_session_show_summary(void) { return s_cfg_detail_summary; }
bool app_session_show_elapsed(void) { return s_cfg_detail_elapsed; }

void app_session_apply_config(bool mask_titles, bool detail_summary, bool detail_elapsed)
{
    s_cfg_mask_titles = mask_titles;
    s_cfg_detail_summary = detail_summary;
    s_cfg_detail_elapsed = detail_elapsed;
}

void app_session_request_list(void)
{
    (void)ap_link_send_json("{\"type\":\"" AP_MSG_SESSION_LIST_REQ "\"}", true);
}

void app_session_request_detail(void)
{
    const ap_session_list_t *list = &s_sessions;
    if (!list->selected[0]) return;
    char json[96];
    snprintf(json, sizeof(json),
             "{\"type\":\"" AP_MSG_SESSION_DETAIL_REQ "\",\"h\":\"%s\"}",
             list->selected);
    (void)ap_link_send_json(json, true);
}

bool app_session_select(const char *handle)
{
    if (!handle || !handle[0]) return false;
    char json[96];
    snprintf(json, sizeof(json),
             "{\"type\":\"" AP_MSG_SESSION_SELECT "\",\"h\":\"%s\"}", handle);
    if (ap_link_send_json(json, true) != ESP_OK) return false;
    (void)ap_session_set_selected(&s_sessions, handle);
    app_session_request_detail();
    return true;
}

// session.list：{n, handles, titles, states, unread, sel}（扁平字段，见 app_proto.h）
static void handle_session_list(const char *json)
{
    char handles[64] = { 0 };
    char titles[512] = { 0 };
    char states[96] = { 0 };
    char unread[32] = { 0 };
    char sel[8] = { 0 };
    ap_json_str(json, "handles", handles, sizeof(handles));
    ap_json_str(json, "titles", titles, sizeof(titles));
    ap_json_str(json, "states", states, sizeof(states));
    ap_json_str(json, "unread", unread, sizeof(unread));
    ap_json_str(json, "sel", sel, sizeof(sel));

    if (!ap_session_list_apply(&s_sessions, handles, titles, states, unread, sel)) {
        ESP_LOGW(TAG, "会话列表解码失败（条数超上限或载荷非法），已清空");
        return;
    }
    ESP_LOGI(TAG, "会话列表：%d 条（选中 %s）", s_sessions.count,
             s_sessions.selected[0] ? s_sessions.selected : "-");

    ap_session_row_t rows[AP_SESSION_MAX];
    for (int i = 0; i < s_sessions.count; i++) rows[i] = s_sessions.rows[i];
    app_screens_post_sessions(rows, s_sessions.count, s_sessions.selected);
}

// session.detail：{h, title, state, step, stepTotal, tool, elapsedMs, summary}
static void handle_session_detail(const char *json)
{
    char h[8] = { 0 };
    char title[96] = { 0 };
    char state[16] = { 0 };
    char tool[24] = { 0 };
    char summary[160] = { 0 };
    long step = -1;
    long step_total = 0;
    long elapsed = 0;
    ap_json_str(json, "h", h, sizeof(h));
    ap_json_str(json, "title", title, sizeof(title));
    ap_json_str(json, "state", state, sizeof(state));
    ap_json_str(json, "tool", tool, sizeof(tool));
    ap_json_str(json, "summary", summary, sizeof(summary));
    if (!ap_json_num(json, "step", &step)) step = -1;
    if (!ap_json_num(json, "stepTotal", &step_total)) step_total = 0;
    if (!ap_json_num(json, "elapsedMs", &elapsed)) elapsed = 0;

    ap_session_detail_apply(&s_session_detail, h, title, state,
                            (int)step, (int)step_total, tool,
                            elapsed > 0 ? (uint32_t)elapsed : 0,
                            summary);
    if (s_session_detail.valid) {
        ESP_LOGI(TAG, "会话详情：%s %s（step=%d/%d）", s_session_detail.handle,
                 ap_session_state_name(s_session_detail.state),
                 s_session_detail.step, s_session_detail.step_total);
        app_screens_post_session_detail(&s_session_detail);
    }
}

// session.done：{h, title, result, durationMs} —— 本地先收敛状态/未读，界面即时反馈。
static void handle_session_done(const char *json)
{
    char h[8] = { 0 };
    char title[AP_SESSION_TITLE_MAX] = { 0 };
    char result[16] = { 0 };
    long duration = 0;
    ap_json_str(json, "h", h, sizeof(h));
    ap_json_str(json, "title", title, sizeof(title));
    ap_json_str(json, "result", result, sizeof(result));
    ap_json_num(json, "durationMs", &duration);

    ap_session_apply_done(&s_sessions, h, result);

    const char *label = "完成";
    bool urgent = false;
    if (strcmp(result, "error") == 0) {
        label = "出错";
        urgent = true;
    } else if (strcmp(result, "aborted") == 0) {
        label = "中断";
    }
    alert_kind_t sound = ALERT_COMPLETED;
    if (strcmp(result, "error") == 0) sound = ALERT_ERROR;
    else if (strcmp(result, "aborted") == 0) sound = ALERT_ABORTED;
    alert_play(sound, alert_get_setting());

    char text[96];
    if (title[0]) snprintf(text, sizeof(text), "%s：%s", label, title);
    else snprintf(text, sizeof(text), "%s", label);
    // TODO(S5b): 改成非模态完成横幅 + 对应提示音；当前先用 toast 保证有反馈。
    app_ui_post_toast(text, urgent);
    ESP_LOGI(TAG, "会话结束：%s %s（%ldms）", h[0] ? h : "?", result, duration);
}

static void process_message(const char *json)
{
    char type[48];
    if (!ap_json_str(json, "type", type, sizeof(type))) return;

    if (strcmp(type, AP_MSG_HELLO_ACK) == 0) {
        // 握手闭环：链路进入 READY 后 ap_link 自己会置状态。
        // 精简版不请求任务列表 —— 状态由主机主动推送（见 handle_task_state）。
        ESP_LOGI(TAG, "握手完成");
        // 协议 v2：握手后主动拉一次会话列表（主机随后会持续推送增量）。
        app_session_request_list();
        return;
    }
    if (strcmp(type, AP_MSG_TASK_STATE) == 0) {
        handle_task_state(json);
        return;
    }
    if (strcmp(type, AP_MSG_CONFIG) == 0) {
        handle_config(json);
        return;
    }
    if (strcmp(type, AP_MSG_SOUND_TEST) == 0) {
        // 桌面设置页「试听」：按当前音量播放指定音型。静音时明确提示，避免"点了没反应"。
        char kind_name[24] = { 0 };
        ap_json_str(json, "kind", kind_name, sizeof(kind_name));
        alert_kind_t kind;
        if (alert_kind_from_name(kind_name, &kind)) {
            if (alert_get_volume() == 0) {
                app_ui_post_toast("提示音已静音", true);
            } else {
                alert_play(kind, true);
            }
            ESP_LOGI(TAG, "试听提示音：%s（音量 %u%%）", kind_name, (unsigned)alert_get_volume());
        } else {
            ESP_LOGW(TAG, "sound.test 未知音型：%s", kind_name);
        }
        return;
    }
    if (strcmp(type, AP_MSG_SESSION_LIST) == 0) {
        handle_session_list(json);
        return;
    }
    if (strcmp(type, AP_MSG_SESSION_DETAIL) == 0) {
        handle_session_detail(json);
        return;
    }
    if (strcmp(type, AP_MSG_SESSION_DONE) == 0) {
        handle_session_done(json);
        return;
    }
    if (strcmp(type, AP_MSG_DEBUG_KEY) == 0) {
        // 把一条按键事件喂进与真实按键同一个队列 —— 测的是真实路径。
        long btn = 0, ev = 0;
        if (ap_json_num(json, "btn", &btn) && ap_json_num(json, "event", &ev)) {
            post_key((uint8_t)btn, (uint8_t)ev);
            ESP_LOGD(TAG, "注入按键 btn=%ld ev=%ld", btn, ev);
        }
        return;
    }
    if (strcmp(type, AP_MSG_TOAST) == 0) {
        handle_toast(json);
        return;
    }
    if (strcmp(type, AP_MSG_BALANCE) == 0) {
        handle_balance(json);
        return;
    }
    if (strcmp(type, AP_MSG_APPROVE_REQ) == 0) {
        handle_approval_req(json);
        return;
    }
    if (strcmp(type, AP_MSG_APPROVE_RESULT) == 0) {
        handle_approval_result(json);
        return;
    }
    if (strcmp(type, AP_MSG_VOICE_RESULT) == 0) {
        // 识别结果卡（docs/06 §2）：带 resultId 就弹结果卡等按键
        // （单击填入 / 双击发送 / 按住重说）；老主机不带 resultId 时退回 toast。
        //
        // ★ 512 字节而不是 120：识别结果是动态长句（一句话几十个汉字很常见），
        //   120 字节（约 40 个汉字）会把长句截断。app_ui_post_toast / 结果卡内部
        //   还会做 UTF-8 安全截断，因此这里只需保证缓冲够大、不再自己截半截。
        char result_id[48] = { 0 };
        char text[512] = { 0 };
        char target[AP_SESSION_HANDLE_MAX] = { 0 };
        ap_json_str(json, "resultId", result_id, sizeof(result_id));
        ap_json_str(json, "text", text, sizeof(text));
        ap_json_str(json, "h", target, sizeof(target));
        if (!text[0]) return;
        // 追问页内的"按住说补充"优先消费（custom 录入），不弹识别结果卡
        if (app_question_on_voice_result(text)) return;
        if (result_id[0]) {
            app_screens_post_voice_result(result_id, text, target);
        } else {
            app_ui_post_toast(text, false);
        }
        return;
    }
    if (strcmp(type, AP_MSG_VOICE_ERROR) == 0) {
        char err[256] = { 0 };
        ap_json_str(json, "message", err, sizeof(err));
        // ★ 诊断：记录收到的 voice.error 原始内容，定位"闪过识别失败"的来源。
        ESP_LOGW(TAG, "收到 voice.error：%s", json);
        app_ui_post_toast(err[0] ? err : "语音识别失败", true);
        return;
    }
    if (strcmp(type, AP_MSG_QUESTION_REQ) == 0) {
        // 追问/计划评审（docs/06 §3/§4）。载荷是拍平的标量（设备端不解析嵌套数组）：
        // options 用 '|' 分隔、multiSelect/hasCustom 用 0/1。
        ap_question_msg_t q;
        memset(&q, 0, sizeof(q));
        ap_json_str(json, "callId", q.call_id, sizeof(q.call_id));
        ap_json_str(json, "qid", q.qid, sizeof(q.qid));
        // ★ 只收一行极短摘要：完整题干/计划正文在电脑卡片上（用户决策 2026-10-06）。
        //   summary 是新字段；回退 question 兼容旧主机。
        if (!ap_json_str(json, "summary", q.summary, sizeof(q.summary))) {
            ap_json_str(json, "question", q.summary, sizeof(q.summary));
        }
        ap_json_str(json, "approveLabel", q.approve_label, sizeof(q.approve_label));
        ap_json_str(json, "options", q.options_csv, sizeof(q.options_csv));
        char kind[8] = { 0 };
        ap_json_str(json, "kind", kind, sizeof(kind));
        q.is_plan = (strcmp(kind, "plan") == 0);
        long n = 0;
        if (ap_json_num(json, "recommendedIndex", &n)) q.recommended_index = (int)n;
        n = 0;
        if (ap_json_num(json, "index", &n)) q.index = (int)n;
        n = 0;
        if (ap_json_num(json, "total", &n)) q.total = (int)n;
        n = 0;
        if (ap_json_num(json, "multiSelect", &n)) q.multi_select = (n != 0);
        n = 0;
        if (ap_json_num(json, "timeoutMs", &n)) q.timeout_ms = n;
        // 提示音只在本批第一题响一次（question.req 是逐题下发的）。
        if (q.index == 0) alert_play(ALERT_QUESTION, alert_get_setting());
        app_screens_post_question(&q);
        return;
    }
    if (strcmp(type, AP_MSG_QUESTION_DONE) == 0) {
        char call_id[48] = { 0 };
        char reason[48] = { 0 };
        long ok = 0;
        ap_json_str(json, "callId", call_id, sizeof(call_id));
        ap_json_str(json, "reason", reason, sizeof(reason));
        ap_json_num(json, "ok", &ok);
        app_screens_post_question_done(call_id, ok != 0, reason);
        return;
    }
    ESP_LOGD(TAG, "未处理的消息类型：%s", type);
}

// ── 按键（中断上下文 → 队列 → app 任务）───────────────────────────────────

/** 入队并**立刻唤醒 app 任务**：按键响应不再是 100ms 轮询粒度。
 *  只做入队 + 任务通知，不阻塞、不碰 LVGL（esp_timer 共享任务里跑）。 */
static void post_key(uint8_t btn, uint8_t event)
{
    if (!s_key_queue) return;
    const key_msg_t msg = { .btn = btn, .event = event };
    if (xQueueSend(s_key_queue, &msg, 0) != pdTRUE) {
        // 队列满丢键 = 用户侧"按了没反应"：必须可见，不许静默
        ESP_LOGW(TAG, "按键队列已满，丢弃 btn=%u ev=%u", (unsigned)btn, (unsigned)event);
        return;
    }
    if (s_app_task) xTaskNotifyGive(s_app_task);
}

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    // ★ 这行日志是"按键到底有没有被驱动识别"的唯一观测点。
    //   之前这里完全没有输出，于是"BSP 没检测到按键"与"检测到了但界面没响应"
    //   这两种完全不同的原因无法区分。
    ESP_LOGD(TAG, "按键事件 btn=%d ev=%d", (int)btn, (int)ev);
    post_key((uint8_t)btn, (uint8_t)ev);
}

/** 处理一个按键事件。返回 false = 没能处理（LVGL 锁超时，事件已丢弃并告警）。 */
static bool handle_key(const key_msg_t *key)
{
    s_last_activity_ms = now_ms();   // 任何按键都算"用户在动"

    // 覆盖层打开时，长按确定 = 返回上一级（由 app_screens 消费），不录音不断开。
    // ★ 锁超时不静默丢键：重试一次，仍拿不到才放弃并告警 ——
    //   丢键的表现是"按了没反应"，而锁超时恰恰在屏幕忙时最容易发生。
    if (!bsp_lvgl_lock(200) && !bsp_lvgl_lock(200)) {
        ESP_LOGW(TAG, "LVGL 锁超时，丢弃按键 btn=%d ev=%d", (int)key->btn, (int)key->event);
        return false;
    }

    const bool consumed = app_ui_handle_key(key->btn, key->event);
    const ap_screen_t screen = app_screens_current();
    ESP_LOGD(TAG, "按键 btn=%d ev=%d 消费=%d 覆盖层=%d",
             (int)key->btn, (int)key->event, (int)consumed, (int)screen);

    // 主页：PTT 按住确定说话 —— 按下开始录音，松开结束并发送。
    // 断开连接已移除 —— 断开只由插件/控制面板那边控制，设备端不再提供。
    if (!consumed && screen == AP_SCREEN_NONE && key->btn == BSP_BTN_OK) {
        if (key->event == BSP_BTN_PRESS) {
            ESP_LOGI(TAG, "按下确定：开始录音");
            app_voice_start();
        } else if (key->event == BSP_BTN_RELEASE) {
            // 不前置判断 recording()：快速点按时任务可能还没醒（state 仍 IDLE），
            // app_voice_stop 内部用 stop_pending 兜住这个窗口。
            ESP_LOGI(TAG, "松开确定");
            app_voice_stop();
        }
    }

    // 上键长按 = 系统设置（仅主页手势；覆盖层打开时长按上键是"返回上一级"）
    if (screen == AP_SCREEN_NONE && key->btn == BSP_BTN_UP && key->event == BSP_BTN_LONG) {
        app_screens_show_settings();
    }

    // 下键长按 = 看余额
    if (screen == AP_SCREEN_NONE && key->btn == BSP_BTN_DOWN && key->event == BSP_BTN_LONG) {
        // 标记"用户请求"：余额数据到达时才弹余额页。主机连上后会主动推余额，
        // 不能一收到就弹（那会让设备在连接完成时自己跳到余额页）。
        app_screens_request_balance();
        (void)ap_link_send_json("{\"type\":\"" AP_MSG_BALANCE_REQ "\"}", true);
    }

    bsp_lvgl_unlock();
    return true;
}

// ── app 任务 ──────────────────────────────────────────────────────────────

// 设备自检信息的刷新周期。电量变化很慢，RSSI 也没必要刷太快 ——
// 每次刷新都要写 LVGL 对象，10 秒一次足够，也省电。
#define DEVICE_INFO_INTERVAL_MS 10000

/**
 * 无任务自动休眠（设置页可配，**默认不休眠**）：
 * 本板没有软件可控的电源锁存，所以"休眠"落地为**深睡** —— 功耗降到最低、
 * 且由 auto_sleep_gate 保证不会自己回来（只有按键才唤醒）。
 *
 * 唤醒源是按键：三个键共用一个 ADC 分压节点（GPIO0），按下即把节点拉低。
 * 深睡前的外设顺序与 demo_low_power 一致 —— BSP 侧的寄存器读回与重试
 * 由 tests/test_deep_sleep_contract.py 守住，这里只负责调用顺序。
 */
static void auto_sleep_now(void)
{
    ESP_LOGW(TAG, "无任务空闲达到设定时长，自动休眠（按任意键唤醒）");
    // 休眠标记 + RTC 魔数：醒来时据此判断是不是用户按键唤醒（见 auto_sleep_gate）
    (void)ap_settings_set_powered_off(true);
    s_power_off_magic = POWER_OFF_MAGIC;
    // 睡前记一下唤醒引脚电平：松开应为高（约 3.3V）。若这里是低，说明按下状态或
    // 分压异常 —— 那种情况下设备会被立刻唤醒，日志里一眼能看出来。
    ESP_LOGI(TAG, "睡前状态：唤醒引脚 GPIO%d 电平=%d（1=高）分压=%d mV 魔数=0x%08X",
             BSP_BTN_ADC_GPIO, gpio_get_level((gpio_num_t)BSP_BTN_ADC_GPIO),
             bsp_button_read_mv(), (unsigned)s_power_off_magic);

    (void)bsp_battery_sleep();          // CW2017 与 ES8311 共用 I2C，必须先停电量计
    (void)bsp_audio_sleep();            // ES8311 进入低功耗
    (void)bsp_audio_prepare_deep_sleep();   // 释放 I2S 引脚
    (void)bsp_i2c_prepare_deep_sleep();     // 释放共用 I2C 引脚

    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "休眠前无法停 LVGL 刷屏，重启恢复外设");
        esp_restart();
    }
    (void)bsp_display_prepare_deep_sleep(); // ST7789 挂起
    backlight_hold_off();                   // 再按住背光引脚（深睡期间 LEDC 掉电会悬空）

    // 按键唤醒：分压节点按下即拉低（三个键都会把它拉到逻辑低电平）。
    // 先把它从 ADC 交回 RTC IO 并上拉 —— 否则会周期性误唤醒（见 wake_pin_prepare）。
    wake_pin_prepare();
    const esp_err_t err = esp_deep_sleep_enable_gpio_wakeup(1ULL << BSP_BTN_ADC_GPIO,
                                                            ESP_GPIO_WAKEUP_GPIO_LOW);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "按键唤醒配置失败：%s（只能重新上电唤醒）", esp_err_to_name(err));
    }
    esp_deep_sleep_start();
    // 深睡准备接口返回后总线已不可在本次运行中恢复
    ESP_LOGE(TAG, "休眠（深睡）意外返回，重启恢复外设");
    esp_restart();
}

static void app_task(void *arg)
{
    (void)arg;
    uint32_t last_info_ms = 0;
    app_msg_t msg;
    key_msg_t key;

    while (s_running) {
        // 1) 链路维护：发送队列、重传、心跳判定
        ap_link_tick();

        // 2) 下行消息：**全部排空**。之前每圈只处理一条，主机批量推送
        //    （question.req 连发等）会被 100ms 轮询串行成 10 条/秒。
        while (xQueueReceive(s_msg_queue, &msg, 0) == pdTRUE) {
            process_message(msg.json);
        }

        // 3) 按键：全部排空
        while (xQueueReceive(s_key_queue, &key, 0) == pdTRUE) {
            (void)handle_key(&key);
        }

        // 4) 自检信息（电量 / RSSI / MTU）
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        // last_info_ms==0 表示首圈：立刻采一次，否则开机 10 秒内的连接
        // 在 hello 里拿不到电量（主机面板的"设备电量"会一直显示"—"）。
        if (last_info_ms == 0 || (uint32_t)(now - last_info_ms) >= DEVICE_INFO_INTERVAL_MS) {
            last_info_ms = now;
            const int soc = bsp_battery_soc();
            if (soc >= 0) s_battery_percent = soc;
            int8_t rssi = 0;
            uint16_t mtu = 0;
            ap_link_peer_info(&rssi, &mtu);
            // 电量喂给链路缓存：hello 握手时随包上报给主机面板。
            ap_link_set_battery(s_battery_percent);
            app_ui_set_device_info(s_battery_percent, rssi, mtu);
        }

        // ── 无任务自动休眠（默认关闭；有任务/覆盖页开着/正在录音都不休眠）──
        const ap_settings_t *set = ap_settings_get();
        if (set->auto_off_min > 0) {
            const bool task_running = (s_task_state == AP_TASK_STATE_RUNNING ||
                                       s_task_state == AP_TASK_STATE_WAITING_APPROVAL);
            const bool overlay_open = app_screens_current() != AP_SCREEN_NONE;
            if (ap_settings_should_power_off(now_ms() - s_last_activity_ms, set->auto_off_min,
                                             task_running, overlay_open, app_voice_recording())) {
                auto_sleep_now();
            }
        }

        // ★ 事件驱动等待（不再是固定 vTaskDelay(100)）：
        //   · 按键经 post_key() 发任务通知 → 立刻醒来处理，按键到画面更新
        //     的延迟从"最多 100ms"降到几毫秒，选题/多选连按才跟手；
        //   · 没有事件时最多睡 100ms —— ap_link_tick() 的链路维护节奏
        //     （重传、心跳判定）保持不变。
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    }
    vTaskDelete(NULL);
}

// ── 入口 ──────────────────────────────────────────────────────────────────

// 注意函数名不是 app_main：ESP-IDF 的入口只能有一个，它仍在上游 main.c 里。
// main.c 只多两行来调用本函数 —— 这是本应用对上游文件的**唯一**侵入点，
// 删掉那两行即可回到 demo 菜单。
void passport_app_main(void)
{
    ESP_LOGI(TAG, "AI Passport × DSH 联机应用启动");

    // NVS 必须在蓝牙之前初始化。否则 PHY 校准数据无处存放，启动日志会打出
    // "esp_phy_load_cal_data_from_nvs: NVS has not been initialized"。
    // 功能上不影响（协议栈内部会兜底），但日志不干净会掩盖真正的问题。
    // 版本不匹配等错误按上游惯例擦除重建，仍失败则继续（BLE 通常照样能用）。
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        (void)nvs_flash_erase();
        nvs_err = nvs_flash_init();
    }

    // ★ 自动休眠门控放在**最前面**（界面/链路/背光都还没起来）：
    //   误唤醒时立刻睡回去，屏幕不亮、蓝牙不广播 —— 用户看到的就是"关着"。
    //   早先放在界面创建之后，每次误唤醒都会闪一下界面，看起来像"自己重启了"（真机反馈）。
    (void)ap_settings_init();

    // 按键队列与按键驱动必须在门控之前就绪：门控要读"唤醒瞬间按键是否真的按着"。
    // 队列提前建好，否则这一小段的按键事件会被丢掉（不影响功能，但没必要）。
    if (!s_key_queue) s_key_queue = xQueueCreate(8, sizeof(key_msg_t));
    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "按键初始化失败（界面仍可用，但无法操作）");
    }

    auto_sleep_gate();
    // 配对状态（设备密钥 / token）与本次上电的配对码：链路起来之前就绪
    (void)ap_pair_init();
    alert_set_volume((uint8_t)ap_settings_get()->alert_volume);
    if (nvs_err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 初始化失败: %s（蓝牙校准数据将不持久化）", esp_err_to_name(nvs_err));
    }

    bsp_i2c_init();

    // 深睡期间背光引脚是 hold 住的，这里先释放，否则醒来屏幕一直黑
    backlight_hold_release();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败，无法继续");
        return;
    }
    bsp_display_backlight(100);

    // ★ 音频外设必须初始化，否则 bsp_audio_write 全部失败 —— 提示音、语音回放
    //   一概没声。之前只有 demo 路径（main.c）调了 bsp_audio_init()，
    //   本应用的 passport_app_main() 漏了它，导致"音频整体没声、提示音从没响过"。
    //   失败不致命（照旧是"没声音"，但界面与链路照常工作）。
    if (bsp_audio_init() != ESP_OK) {
        ESP_LOGW(TAG, "音频初始化失败，提示音与语音回放将不可用");
    }

    s_msg_queue = xQueueCreate(APP_MSG_QUEUE_DEPTH, sizeof(app_msg_t));
    if (!s_key_queue) s_key_queue = xQueueCreate(8, sizeof(key_msg_t));
    if (!s_msg_queue || !s_key_queue) {
        ESP_LOGE(TAG, "队列创建失败");
        return;
    }

    // 消息回调必须在 start 之前注册：否则启动瞬间的握手消息会丢。
    ap_link_set_message_cb(on_link_message, NULL);
    ap_link_set_state_cb(on_link_state, NULL);

    s_running = true;
    if (xTaskCreate(app_task, "app_task", 6144, NULL, 5, &s_app_task) != pdPASS) {
        ESP_LOGE(TAG, "app 任务创建失败");
        s_running = false;
        return;
    }

    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "电量计初始化失败，电量显示为 --");
    } else {
        // 预热一次：让 hello 在首个 10 秒窗口内也有电量可报。
        const int soc = bsp_battery_soc();
        if (soc >= 0) s_battery_percent = soc;
        ap_link_set_battery(s_battery_percent);
    }

    // 先建界面再启动 BLE：这样第一个状态回调一定能刷到屏幕上。
    if (bsp_lvgl_lock(1000)) {
        app_ui_create();
        bsp_lvgl_unlock();
    } else {
        ESP_LOGE(TAG, "取 LVGL 锁失败，界面未能创建");
    }

#if CONFIG_APP_ENABLE_SCREENSHOT
    // 串口截屏：开发/验收基础设施，默认关闭（整屏缓冲 150KB 放不下，
    // 见 main/Kconfig.projbuild 的说明）。开启时必须在界面建好之后启动，
    // 否则第一帧抓到的是空屏。
    if (app_screenshot_start() != ESP_OK) {
        ESP_LOGW(TAG, "串口截屏未启用（不影响其它功能）");
    }
#endif

    // 按键驱动已在门控之前初始化（见 app_main 开头）

    // 提示音：在界面之后初始化，失败不影响界面与链路
    // 系统设置（阶段 B）：从 NVS 读设置并落地音量。
    // 读失败/首次开机都会回落到默认值（常亮 / 不休眠 / 中音量），不阻塞启动。
    if (alert_init() != ESP_OK) {
        ESP_LOGW(TAG, "提示音初始化失败（界面仍可用，但没有声音提醒）");
    }

    // 语音上行（PTT 录音）：失败只影响语音，界面与链路照常
    if (app_voice_init() != ESP_OK) {
        ESP_LOGW(TAG, "语音上行初始化失败（按住确定不会录音）");
    }

    const esp_err_t link_err = ap_link_start();
    if (link_err != ESP_OK) {
        ESP_LOGE(TAG, "BLE 链路启动失败: %s", esp_err_to_name(link_err));
        app_ui_post_toast("蓝牙启动失败", true);
    } else {
        ESP_LOGI(TAG, "蓝牙已启动，广播名 %s", AP_DEVICE_NAME);
    }
}
