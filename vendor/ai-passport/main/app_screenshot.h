// main/app_screenshot.h —— FAP_SCREENSHOT_V1 串口截屏（开发/验收基础设施）。
//
// 默认在固件里启用：没有它，界面问题只能靠"盲改 + 用户口头反馈"。
// 发布正式玩法前可以按需关掉（见 app.c 里的调用点）。
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 安装 USB-Serial-JTAG 驱动并启动监听任务。可重复调用（已启动返回 INVALID_STATE）。
esp_err_t app_screenshot_start(void);

// 驱动是否确实装好、任务已在跑。启动是异步的，因此不要用返回值当"可用"。
bool app_screenshot_ready(void);

#ifdef __cplusplus
}
#endif
