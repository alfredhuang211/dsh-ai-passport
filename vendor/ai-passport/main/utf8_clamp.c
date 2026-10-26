// main/utf8_clamp.c —— UTF-8 安全截断实现。
//
// ★ 为什么不能用 snprintf("%s") 或按字节 memcpy 后直接截断：
//   UTF-8 汉字是 3 字节，按字节硬切会切在字符中间，产生非法字节序列。
//   LVGL 遇到非法序列按缺失字形处理，屏幕上就出现**半截字/方块** ——
//   这正是"文字不全，混着方块"的根因之一。
//
// 做法：先按字节复制到 dst_size-1，再**回退到最近的完整 UTF-8 字符边界**，
// 保证 dst 里永远是完整字符。
#include "utf8_clamp.h"

#include <string.h>

void utf8_copy_clamped(const char *src, char *dst, size_t dst_size)
{
    if (dst_size == 0) return;
    if (!src) { dst[0] = '\0'; return; }

    size_t len = strnlen(src, dst_size - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';

    // ★ 回退到最近的完整 UTF-8 字符边界。
    //
    //   之前的写法在"去掉一个 continuation byte 后立刻 break"，结果只删了
    //   半截汉字的最后一个字节，仍留下前两个字节 —— 照样是非法序列，照样出方块。
    //   正确做法：从末尾**连续**跳过所有 continuation byte（0b10xxxxxx），
    //   落到字符的起始字节上，再判断这个起始字节声明的完整序列长度
    //   是否真的放得下；放不下就整段删掉，放得下就停在这里。
    while (len > 0) {
        // 1) 跳过末尾所有 continuation byte，找到字符起始字节
        size_t start = len;
        while (start > 0 && ((unsigned char)dst[start - 1] & 0xC0) == 0x80) {
            start--;
        }
        if (start == 0) {
            // 整个缓冲全是 continuation byte（极端情况）→ 全清空
            len = 0;
            break;
        }
        // 2) dst[start-1] 是某个字符的起始字节；看它声明的序列长度够不够
        const unsigned char lead = (unsigned char)dst[start - 1];
        int need = 1;
        if ((lead & 0xE0) == 0xC0) need = 2;
        else if ((lead & 0xF0) == 0xE0) need = 3;
        else if ((lead & 0xF8) == 0xF0) need = 4;
        if (len - (start - 1) >= (size_t)need) {
            break;   // 这个字符完整，保留
        }
        // 这个字符被截断了 → 整段删掉，继续往更早的字符找
        len = start - 1;
    }
    dst[len] = '\0';
}
