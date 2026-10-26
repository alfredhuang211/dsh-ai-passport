// tests/test_utf8_clamp.c —— UTF-8 安全截断的 host 测试。
//
// 为什么需要它：截断错了会"在汉字中间切断"，产生非法 UTF-8 字节，
// LVGL 渲染成方块（正是"文字不全，混着方块"的根因）。
// 这种 bug 只在真机上看得见，必须在 host 侧提前拦住。
#include "utf8_clamp.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

// 检查字符串是否是合法 UTF-8（无半截字符）。
static int is_valid_utf8(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int need;
        if (*p < 0x80) need = 0;
        else if ((*p & 0xE0) == 0xC0) need = 1;
        else if ((*p & 0xF0) == 0xE0) need = 2;
        else if ((*p & 0xF8) == 0xF0) need = 3;
        else return 0;   // 非法起始字节
        p++;
        for (int i = 0; i < need; i++) {
            if ((*p & 0xC0) != 0x80) return 0;   // 缺 continuation byte
            p++;
        }
    }
    return 1;
}

static void expect(const char *src, size_t buf_size, const char *want, const char *label)
{
    char dst[64];
    memset(dst, 0xAA, sizeof(dst));
    utf8_copy_clamped(src, dst, buf_size);
    assert(is_valid_utf8(dst) && "截断结果必须是合法 UTF-8");
    if (strcmp(dst, want) != 0) {
        fprintf(stderr, "✗ %s：期望 '%s'，实际 '%s'\n", label, want, dst);
        assert(0);
    }
    printf("✓ %s\n", label);
}

int main(void)
{
    // 1. 不需要截断：源串比缓冲短
    expect("你好世界", 32, "你好世界", "短串原样复制");

    // 2. NULL / 0 尺寸的边界
    expect(NULL, 32, "", "NULL 源串 → 空串");
    // dst_size=0 时函数直接 return，不写任何东西（连 NUL 都不写）
    char tiny[1];
    memset(tiny, 0xAA, sizeof(tiny));
    utf8_copy_clamped("abc", tiny, 0);
    assert((unsigned char)tiny[0] == 0xAA);

    // 3. 恰好在字符边界截断（汉字 3 字节，buf=10 → 放 3 个汉字 9 字节 + NUL）
    expect("你好世界", 10, "你好世", "3 字节汉字按边界截断");

    // 4. 截断点落在汉字中间（buf=8 → 会切到第 3 个汉字的第 2 字节）
    expect("你好世界", 8, "你好", "截断点落在汉字中间 → 回退到完整字符");

    // 5. 只有 1 字节缓冲：装不下任何汉字 → 空串
    expect("你好", 1, "", "1 字节缓冲装不下汉字 → 空串");

    // 6. 混合 ASCII + 汉字：buf=7 → "abc你" 是 3+3=6 字节，放得下；再切"好"就超
    expect("abc你好", 7, "abc你", "混合串按字符边界截断");

    // 7. 4 字节 UTF-8（emoji）截断点在中间
    // "😀" = F0 9F 98 80（4 字节），buf=3 → 只能放 2 字节 → 全部回退 → 空串
    expect("\xF0\x9F\x98\x80", 3, "", "4 字节 emoji 被截断 → 空串");

    // 8. 4 字节字符完整保留
    expect("\xF0\x9F\x98\x80", 5, "\xF0\x9F\x98\x80", "4 字节 emoji 完整保留");

    // 9. 空串
    expect("", 8, "", "空串");

    printf("\n全部 UTF-8 截断测试通过\n");
    return 0;
}
