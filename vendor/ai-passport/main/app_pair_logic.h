// main/app_pair_logic.h —— 配对的**纯逻辑**（无 NVS / 无 BLE，可在 host 单测）。
//
// 配对是"认领/信任"握手（docs/06 §9.14）：
//   设备屏幕显示一次性 6 位码 → 主机输码 → 设备校验 → 生成 token 存 NVS → 之后 hello 带 token。
// 这里只放可单测的部分：码派生、常量时间比较、失败限流。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AP_PAIR_CODE_DIGITS 6
#define AP_PAIR_SECRET_BYTES 16
#define AP_PAIR_TOKEN_BYTES 16
#define AP_PAIR_TOKEN_HEX (AP_PAIR_TOKEN_BYTES * 2)

/** 每分钟最多尝试次数（防暴力猜 6 位码）。 */
#define AP_PAIR_MAX_ATTEMPTS 3
#define AP_PAIR_RATE_WINDOW_MS 60000u

/**
 * 由设备密钥 + 本次上电随机数派生 6 位配对码（每次上电不同）。
 *
 * 输出恒为 6 位十进制、允许前导零，调用方保证 out 至少 AP_PAIR_CODE_DIGITS+1 字节。
 */
void ap_pair_code(const uint8_t *secret, size_t secret_len, uint32_t nonce,
                  char out[AP_PAIR_CODE_DIGITS + 1]);

/** 常量时间比较配对码；长度不符直接 false（长度不是秘密）。 */
bool ap_pair_code_equal(const char *expected, const char *provided);

/** 常量时间比较 token（十六进制，大小写不敏感）；长度不符或含非十六进制字符→false。 */
bool ap_pair_token_equal(const char *expected_hex, const char *provided_hex);

/** 尝试限流状态（设备侧只留一份）。 */
typedef struct {
    uint32_t window_start_ms;
    uint8_t attempts;
} ap_pair_rate_t;

/** 当前是否还允许尝试（窗口过期自动放行）。 */
bool ap_pair_rate_allow(const ap_pair_rate_t *rate, uint32_t now_ms);

/** 记一次失败（内部处理窗口滚动）。 */
void ap_pair_rate_record_failure(ap_pair_rate_t *rate, uint32_t now_ms);

/** 配对成功后清空限流状态。 */
void ap_pair_rate_reset(ap_pair_rate_t *rate);
