// main/app_screenshot.c —— FAP_SCREENSHOT_V1：经 USB 串口抓取屏幕。
//
// ★ 这是**开发/验收基础设施**，不是面向用户的功能。存在的理由很直接：
//   设备没有网络也没有调试器，界面问题（中文字形、布局溢出、颜色）只能靠"看见屏幕"来判断。
//   没有它就只能靠用户口头描述"显示有点问题"，然后盲改 —— 那是最低效的调试方式。
//
// 协议（与 AI Passport 社区发布流程一致）：
//   主机 → 设备：ASCII 行 "FAP_SCREENSHOT_V1\n"
//   设备 → 主机："FAP_SCREENSHOT_V1 <宽> <高> RGB565LE <字节数>\n" + 紧排的像素字节
//
// 实现要点全部来自上游 docs/reference/y2lin/serial-screenshot-protocol.zh_CN.md
// 记录的真实踩坑，逐条都不省略：
//
//  1) **先装驱动**。直接进应用界面的固件不启动 REPL，若不调 usb_serial_jtag_driver_install()
//     就读串口，会解引用空驱动对象 —— 本板上表现为反复崩溃重启，而背光已亮，
//     用户看到的是"屏幕一直在闪"，完全没有串口报错。极难从现象反推。
//  2) **读任务不能忙等**。持续读错误时紧凑循环会打满任务、饿死 IDLE 并触发看门狗，
//     表现同样是闪屏。读错误要退避，任务优先级要压在 LVGL 之下。
//  3) **按子串匹配命令，不依赖换行**。某些终端会吞掉行尾换行，严格换行匹配永远不命中。
//  4) **整屏缓冲必须静态预留**。无 PSRAM 的芯片上，即使堆里还有 200KB 空闲，
//     也拿不出一块连续的 153,600 字节（LVGL/codec/DMA 起来之后尤其如此）。
//  5) **按发送环形缓冲的容量分块**。usb_serial_jtag_write_bytes 底层是 xRingbufferSend，
//     超过环形缓冲容量的整块写入会立刻失败（"data will never ever fit"）。
//  6) **二进制窗口期间静默一切日志**。日志与像素共用同一条流，混入一个字节整幅图就错位。
//  7) **失败就静默**。命令是只读观察语义：任何失败只记日志、不应答，让主机的超时来报错，
//     而不是发一条被破坏的流。
#include "app_screenshot.h"

#include "bsp_display.h"
#include "bsp_pins.h"

#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "app_shot";

// 命令字面量与应答头。主机侧靠这两个字符串定位数据流。
#define SHOT_CMD "FAP_SCREENSHOT_V1"
#define SHOT_MAGIC "FAP_SCREENSHOT_V1"

// 整屏 RGB565：240 × 320 × 2 = 153,600 字节。
#define SHOT_BYTES ((size_t)BSP_LCD_W * BSP_LCD_H * 2)

// 驱动缓冲。文档实测：rx 256 / tx 1024 就够。
#define SHOT_RX_BUF 256
#define SHOT_TX_BUF 1024
// 每块发送大小：必须**远小于** tx 环形缓冲，否则 xRingbufferSend 直接失败。
#define SHOT_CHUNK 512

// 读任务优先级必须低于 LVGL（LVGL 为 4）。取 3。
#define SHOT_TASK_PRIO 3
#define SHOT_TASK_STACK 8192   // 文档实测：整屏软件渲染 8192 很宽裕

// 整屏 RGB565 缓冲。
//
// ★ 这块缓冲的放置方式试错了两轮，结论值得记下来：
//   1) 运行时从堆申请 → **不可靠**。实测最大连续块只有 25,600 字节：
//      LVGL 内存池 + BLE 协议栈 + 音频栈把 DRAM 切碎了，总空闲够但拿不出连续 150KB
//      （正是上游文档警告的那个坑）。
//   2) 按横带抓取以缩小缓冲 → **行不通**。lv_draw_buf_init 要求缓冲容量能容纳
//      整个 w×h，横带缓冲会被它拒掉，而快照 API 也没有"只渲染某几行"的入口。
//   3) 最终方案：**静态预留整屏缓冲**，并从别处把 150KB 省出来 ——
//      把 LVGL 内存池从 64KB 压回 24KB（见 sdkconfig.defaults 的说明）。
//      这是唯一能让链接器接受、又保证运行时可用的组合。
//
// 静态预留还有个额外好处：地址在编译期就定死，不受运行时碎片影响，
// 每次抓图都必然成功（上游文档也是同一结论）。
static uint8_t s_screen_buf[SHOT_BYTES] __attribute__((aligned(64)));
static lv_draw_buf_t s_draw_buf;

static volatile bool s_ready = false;
static int s_dbg = 0;   // 临时诊断计数
static bool s_driver_installed = false;

// ── 二进制流发送 ──────────────────────────────────────────────────────────

// 带超时地写满一整块。返回 false 表示主机断了，应放弃本次传输。
static bool write_all(const uint8_t *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        const int written = usb_serial_jtag_write_bytes(
            data + sent, len - sent, pdMS_TO_TICKS(1000));
        if (written <= 0) return false;
        sent += (size_t)written;
    }
    return true;
}

// 抓一帧并以协议格式发送。失败时**静默返回**（见文件头第 7 点）。
static void capture_and_send(void)
{
    if (!bsp_lvgl_lock(500)) {
        ESP_LOGW(TAG, "取 LVGL 锁超时，放弃本次截屏");
        return;
    }
    // LVGL 锁只覆盖渲染瞬间；回传期间 UI 照常动画。
    const lv_result_t res = lv_snapshot_take_to_draw_buf(
        lv_screen_active(), LV_COLOR_FORMAT_RGB565, &s_draw_buf);
    bsp_lvgl_unlock();

    if (res != LV_RESULT_OK) {
        ESP_LOGW(TAG, "快照失败，放弃本次截屏");
        return;
    }

    // 防御性核对：必须是紧排的整屏 RGB565，否则发出去的字节数会对不上。
    const uint32_t stride = s_draw_buf.header.stride;
    if (s_draw_buf.header.w != BSP_LCD_W || s_draw_buf.header.h != BSP_LCD_H ||
        s_draw_buf.header.cf != LV_COLOR_FORMAT_RGB565 ||
        stride != (uint32_t)BSP_LCD_W * 2) {
        ESP_LOGW(TAG, "快照格式不符：%ux%u cf=%d stride=%u，放弃",
                 (unsigned)s_draw_buf.header.w, (unsigned)s_draw_buf.header.h,
                 (int)s_draw_buf.header.cf, (unsigned)stride);
        return;
    }

    char header[64];
    const int header_len = snprintf(header, sizeof(header), "%s %d %d RGB565LE %u\n",
                                    SHOT_MAGIC, BSP_LCD_W, BSP_LCD_H, (unsigned)SHOT_BYTES);

    // ★ 二进制窗口开始：静默一切日志，直到最后一个像素字节发完。
    esp_log_level_set("*", ESP_LOG_NONE);

    bool ok = write_all((const uint8_t *)header, (size_t)header_len);
    if (ok) {
        for (size_t off = 0; off < SHOT_BYTES; off += SHOT_CHUNK) {
            size_t n = SHOT_BYTES - off;
            if (n > SHOT_CHUNK) n = SHOT_CHUNK;
            if (!write_all(s_screen_buf + off, n)) {
                ok = false;   // 主机拔线：放弃本次，任务继续存活等下一条命令
                break;
            }
        }
    }

    // 恢复日志级别。传输完成后的日志严格留在这个窗口之外。
    esp_log_level_set("*", ESP_LOG_INFO);
    if (!ok) ESP_LOGW(TAG, "截屏传输中断");
}

// ── 命令识别 ──────────────────────────────────────────────────────────────

// 滑动窗口按子串匹配，不依赖换行终结（见文件头第 3 点）。
static void feed_bytes(const uint8_t *data, int len, char *window, size_t *window_len)
{
    const size_t cmd_len = strlen(SHOT_CMD);
    for (int i = 0; i < len; i++) {
        const char c = (char)data[i];
        // 任何行终结符都重置窗口，避免残留字节连续误触发。
        if (c == '\n' || c == '\r') {
            *window_len = 0;
            continue;
        }
        if (*window_len < cmd_len) {
            window[(*window_len)++] = c;
        } else {
            memmove(window, window + 1, cmd_len - 1);
            window[cmd_len - 1] = c;
        }
        if (*window_len == cmd_len && memcmp(window, SHOT_CMD, cmd_len) == 0) {
            *window_len = 0;   // 命中即清空，防连续误触发
            ESP_LOGW(TAG, "命令命中，开始截屏");
            capture_and_send();
        }
    }
}

// 临时探针：每 500ms 往串口直写一个字符，表示读任务在跑；
// 收到任何输入就写 '+'，读到命令就写 '!'. 直接写 USB，不经日志系统。
static void probe_loop(void)
{
    char c = '.';
    const int n = usb_serial_jtag_read_bytes((void *)&c + 1, 0, 0);  // 仅探测驱动可用性
    (void)n;
    static uint8_t junk[64];
    const int got = usb_serial_jtag_read_bytes(junk, sizeof(junk), 0);
    char out = got > 0 ? '+' : '.';
    usb_serial_jtag_write_bytes(&out, 1, 0);
}

static void shot_task(void *arg)
{
    (void)arg;
    char window[32];
    size_t window_len = 0;
    uint8_t rx[SHOT_RX_BUF];

    for (;;) {
        // 驱动可能尚未就绪（安装是异步的），低频轮询而不是假设可用。
        if (!usb_serial_jtag_is_driver_installed()) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        s_driver_installed = true;

        const int n = usb_serial_jtag_read_bytes(rx, sizeof(rx), pdMS_TO_TICKS(100));
        if (n > 0) {
            // 把收到的原始字节直接回显到串口（不经日志系统，避免被静默窗口吞掉）。
            // 联调时这一行能立刻区分"命令没到"和"设备没答"。
            usb_serial_jtag_write_bytes("[rx]", 4, 0);
            usb_serial_jtag_write_bytes(rx, (size_t)n, 0);
            feed_bytes(rx, n, window, &window_len);
            continue;
        }
        // ★ 关键：驱动提供的 RX ring buffer 只能有**一个**读者。
        //   之前为了诊断加了一个独立探针任务也去 read，两个读者互相抢，
        //   结果谁都读不到（表现为"命令完全没到达"）。诊断探针已并入本任务。
        // 读错误或无事可做：必须退避。紧凑空转会饿死 IDLE 并触发看门狗，
        // 而现象是"屏幕在闪"—— 与驱动缺失的崩溃长得一模一样（见文件头第 2 点）。
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// ── 生命周期 ──────────────────────────────────────────────────────────────

esp_err_t app_screenshot_start(void)
{
    if (s_ready) return ESP_ERR_INVALID_STATE;

    // 注意：usb_serial_jtag_driver_install 的形参不是 const，别加 const（会告警）。
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = SHOT_TX_BUF,
        .rx_buffer_size = SHOT_RX_BUF,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "USB 串口驱动安装失败: %s（截屏不可用）", esp_err_to_name(err));
        return err;
    }
    // 把控制台 VFS 切到驱动通道：否则日志仍走寄存器直写，与我们抢同一条流。
    usb_serial_jtag_vfs_use_driver();

    if (xTaskCreate(shot_task, "app_shot", SHOT_TASK_STACK, NULL, SHOT_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "截屏任务创建失败");
        return ESP_ERR_NO_MEM;
    }

    // 初始化描述符，指向静态预留的整屏缓冲（完全不经过 LVGL 堆）。
    if (lv_draw_buf_init(&s_draw_buf, BSP_LCD_W, BSP_LCD_H, LV_COLOR_FORMAT_RGB565,
                         BSP_LCD_W * 2, s_screen_buf, sizeof(s_screen_buf)) != LV_RESULT_OK) {
        ESP_LOGE(TAG, "lv_draw_buf_init 失败");
        return ESP_FAIL;
    }

    s_ready = true;
    ESP_LOGI(TAG, "串口截屏就绪（发送 \"%s\" 抓取 %ux%u，缓冲 %u 字节）",
             SHOT_CMD, BSP_LCD_W, BSP_LCD_H, (unsigned)SHOT_BYTES);
    return ESP_OK;
}

bool app_screenshot_ready(void)
{
    return s_ready && s_driver_installed;
}
