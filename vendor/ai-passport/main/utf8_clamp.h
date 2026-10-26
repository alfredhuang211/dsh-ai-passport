// main/utf8_clamp.h —— UTF-8 安全截断（纯逻辑，不依赖 ESP-IDF/LVGL）。
//
// 为什么单独成模块：截断逻辑错了会"在汉字中间切断"，产生非法 UTF-8 字节，
// LVGL 渲染成方块。这种 bug 只在真机上看得见，必须有 host 测试提前拦住。
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 把 src 复制进 dst（最多 dst_size-1 字节），并保证 dst 里永远是**完整**的
 * UTF-8 字符序列（不会在多字节字符中间截断）。
 *
 * - dst_size == 0：无操作
 * - src == NULL：写空串
 * - 截断发生在字符边界上，末尾不会残留半截汉字
 */
void utf8_copy_clamped(const char *src, char *dst, size_t dst_size);

#ifdef __cplusplus
}
#endif
