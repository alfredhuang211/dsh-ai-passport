// main/app_pair.h —— 设备端配对状态（NVS 持久）+ 一次性配对码。
//
// 与 app_pair_logic 的分工：逻辑层是纯函数（码派生/比较/限流，可在 host 单测）；
// 本层负责随机数、NVS 读写与"本次上电的码"。
#pragma once

#include <stdbool.h>

#include "app_pair_logic.h"
#include "esp_err.h"

/** 读 NVS（缺 secret 时随机生成），并派生本次上电的配对码。 */
esp_err_t ap_pair_init(void);

/** 设备短 ID（由蓝牙 MAC 派生，形如 "a1b2c3"），用于主机侧信任表。 */
const char *ap_pair_device_id(void);

/** 本次上电的 6 位配对码（屏幕显示用；每次上电都变）。 */
const char *ap_pair_code_str(void);

/** 是否已配对（NVS 里有 token）。 */
bool ap_pair_is_paired(void);

/** 主机 hello 里带的 token 是否匹配（未配对时恒 false）。 */
bool ap_pair_host_token_ok(const char *token_hex);

/** 已配对主机的名字（未配对时为空串）。 */
const char *ap_pair_host_name(void);

/** 当前窗口还剩几次尝试（回 pair.failed 用）。 */
int ap_pair_attempts_left(void);

/**
 * 处理主机 pair.begin：限流 → 常量时间比较配对码 → 成功则随机生成 token 存 NVS。
 *
 * 成功返回 ESP_OK 并填好 out_token；码错/被限流返回非 0（调用方回 pair.failed）。
 */
esp_err_t ap_pair_begin(const char *code, const char *host_name,
                        char out_token[AP_PAIR_TOKEN_HEX + 1]);

/** 解除配对：清 token 与主机名（下次连接需重新配对；重新上电会换新码）。 */
esp_err_t ap_pair_reset(void);
