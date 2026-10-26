// 配对纯逻辑的 host 测试（接进 validate.sh --static）。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "app_pair_logic.h"

static void test_code_derivation(void)
{
    const uint8_t secret[AP_PAIR_SECRET_BYTES] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    char a[AP_PAIR_CODE_DIGITS + 1] = { 0 };
    char b[AP_PAIR_CODE_DIGITS + 1] = { 0 };
    char c[AP_PAIR_CODE_DIGITS + 1] = { 0 };

    ap_pair_code(secret, sizeof(secret), 0x12345678u, a);
    ap_pair_code(secret, sizeof(secret), 0x12345678u, b);
    ap_pair_code(secret, sizeof(secret), 0x12345679u, c);

    assert(strlen(a) == AP_PAIR_CODE_DIGITS);
    for (int i = 0; i < AP_PAIR_CODE_DIGITS; i++) assert(a[i] >= '0' && a[i] <= '9');
    assert(strcmp(a, b) == 0);        // 同密钥同 nonce → 稳定
    assert(strcmp(a, c) != 0);        // 换 nonce → 换码（每次上电不同）

    uint8_t other[AP_PAIR_SECRET_BYTES];
    memcpy(other, secret, sizeof(other));
    other[0] ^= 0xFF;
    char d[AP_PAIR_CODE_DIGITS + 1] = { 0 };
    ap_pair_code(other, sizeof(other), 0x12345678u, d);
    assert(strcmp(a, d) != 0);        // 换密钥 → 换码（换设备就换码）
    printf("== 配对码派生 ==  通过（%s）\n", a);
}

static void test_code_equal(void)
{
    assert(ap_pair_code_equal("012345", "012345"));
    assert(!ap_pair_code_equal("012345", "012346"));
    assert(!ap_pair_code_equal("012345", "01234"));     // 长度不符
    assert(!ap_pair_code_equal("012345", "0123456"));
    assert(!ap_pair_code_equal("012345", ""));
    assert(!ap_pair_code_equal(NULL, "012345"));
    assert(!ap_pair_code_equal("012345", NULL));
    printf("== 配对码比较 ==  通过\n");
}

static void test_token_equal(void)
{
    assert(ap_pair_token_equal("00112233445566778899aabbccddeeff",
                               "00112233445566778899aabbccddeeff"));
    // 大小写不敏感：主机可能回大写
    assert(ap_pair_token_equal("00112233445566778899aabbccddeeff",
                               "00112233445566778899AABBCCDDEEFF"));
    assert(!ap_pair_token_equal("00112233445566778899aabbccddeeff",
                                "00112233445566778899aabbccddeefe"));
    assert(!ap_pair_token_equal("00112233445566778899aabbccddeeff", "00"));      // 长度不符
    assert(!ap_pair_token_equal("zz112233445566778899aabbccddeeff",               // 非法字符
                                "zz112233445566778899aabbccddeeff"));
    assert(!ap_pair_token_equal(NULL, NULL));
    printf("== token 比较 ==  通过\n");
}

static void test_rate_limit(void)
{
    ap_pair_rate_t rate = { 0, 0 };
    const uint32_t t0 = 1000;

    assert(ap_pair_rate_allow(&rate, t0));            // 空状态放行
    ap_pair_rate_record_failure(&rate, t0);
    ap_pair_rate_record_failure(&rate, t0 + 10);
    ap_pair_rate_record_failure(&rate, t0 + 20);
    assert(!ap_pair_rate_allow(&rate, t0 + 30));      // 第 4 次被拒

    assert(ap_pair_rate_allow(&rate, t0 + AP_PAIR_RATE_WINDOW_MS));      // 窗口过期
    ap_pair_rate_record_failure(&rate, t0 + AP_PAIR_RATE_WINDOW_MS + 5);
    assert(ap_pair_rate_allow(&rate, t0 + AP_PAIR_RATE_WINDOW_MS + 10)); // 新窗口重新计数
    assert(rate.attempts == 1);

    ap_pair_rate_reset(&rate);
    assert(rate.attempts == 0);
    assert(ap_pair_rate_allow(&rate, t0 + AP_PAIR_RATE_WINDOW_MS + 20));
    printf("== 尝试限流 ==  通过（每分钟 %d 次）\n", AP_PAIR_MAX_ATTEMPTS);
}

int main(void)
{
    test_code_derivation();
    test_code_equal();
    test_token_equal();
    test_rate_limit();
    printf("== 配对逻辑测试全部通过 ==\n");
    return 0;
}
