// main/app_pair_logic.c —— 见头文件说明。
#include "app_pair_logic.h"

#include <stdio.h>
#include <string.h>

/** FNV-1a：够用的非密码学散列，目的是"由密钥推出稳定码"而不是抗碰撞。 */
static uint32_t fnv1a(const uint8_t *data, size_t len, uint32_t hash)
{
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

void ap_pair_code(const uint8_t *secret, size_t secret_len, uint32_t nonce,
                  char out[AP_PAIR_CODE_DIGITS + 1])
{
    uint32_t h = 2166136261u;
    if (secret && secret_len > 0) h = fnv1a(secret, secret_len, h);
    const uint8_t nonce_bytes[4] = {
        (uint8_t)(nonce & 0xFF), (uint8_t)((nonce >> 8) & 0xFF),
        (uint8_t)((nonce >> 16) & 0xFF), (uint8_t)((nonce >> 24) & 0xFF),
    };
    h = fnv1a(nonce_bytes, sizeof(nonce_bytes), h);
    snprintf(out, AP_PAIR_CODE_DIGITS + 1, "%06u", (unsigned)(h % 1000000u));
}

bool ap_pair_code_equal(const char *expected, const char *provided)
{
    if (!expected || !provided) return false;
    if (strlen(provided) != AP_PAIR_CODE_DIGITS) return false;
    uint8_t diff = 0;
    for (int i = 0; i < AP_PAIR_CODE_DIGITS; i++) {
        diff |= (uint8_t)(expected[i] ^ provided[i]);
    }
    return diff == 0;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    const char lower = (char)(c | 0x20);
    if (lower >= 'a' && lower <= 'f') return lower - 'a' + 10;
    return -1;
}

bool ap_pair_token_equal(const char *expected_hex, const char *provided_hex)
{
    if (!expected_hex || !provided_hex) return false;
    if (strlen(expected_hex) != AP_PAIR_TOKEN_HEX) return false;
    if (strlen(provided_hex) != AP_PAIR_TOKEN_HEX) return false;
    uint8_t diff = 0;
    for (int i = 0; i < AP_PAIR_TOKEN_HEX; i++) {
        const int a = hex_val(expected_hex[i]);
        const int b = hex_val(provided_hex[i]);
        if (a < 0 || b < 0) return false;   // 非法字符不是秘密，允许提前返回
        diff |= (uint8_t)(a ^ b);
    }
    return diff == 0;
}

bool ap_pair_rate_allow(const ap_pair_rate_t *rate, uint32_t now_ms)
{
    if (!rate) return true;
    if (rate->attempts == 0) return true;
    if ((uint32_t)(now_ms - rate->window_start_ms) >= AP_PAIR_RATE_WINDOW_MS) return true;
    return rate->attempts < AP_PAIR_MAX_ATTEMPTS;
}

void ap_pair_rate_record_failure(ap_pair_rate_t *rate, uint32_t now_ms)
{
    if (!rate) return;
    if (rate->attempts == 0 ||
        (uint32_t)(now_ms - rate->window_start_ms) >= AP_PAIR_RATE_WINDOW_MS) {
        rate->window_start_ms = now_ms;
        rate->attempts = 0;
    }
    if (rate->attempts < 255) rate->attempts++;
}

void ap_pair_rate_reset(ap_pair_rate_t *rate)
{
    if (!rate) return;
    rate->window_start_ms = 0;
    rate->attempts = 0;
}
