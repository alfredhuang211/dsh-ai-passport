// tests/test_conn_logic.c —— 连接呈现与重连退避的 host 测试。
//
// 为什么需要它：
//   · RSSI→格数错 = 信号显示撒谎，用户照着它判断要不要挪设备；
//   · 退避错 = 弱信号断开后"永远连不上，只能重启设备"（真机返工记录）。
// 两个都只在真机上发作，必须在 host 侧钉死。
#include "app_conn_logic.h"

#include <assert.h>
#include <stdio.h>

static void test_rssi_level(void)
{
    assert(ap_rssi_level(-40) == 4);
    assert((ap_rssi_level(-60) == 4) && "边界 -60 算满格");
    assert(ap_rssi_level(-61) == 3);
    assert((ap_rssi_level(-70) == 3) && "边界 -70 算 3 格");
    assert(ap_rssi_level(-71) == 2);
    assert(ap_rssi_level(-80) == 2);
    assert(ap_rssi_level(-81) == 1);
    assert(ap_rssi_level(-90) == 1);
    assert((ap_rssi_level(-91) == 0) && "边缘信号 0 格（红），比藏着问题诚实");
    assert(ap_rssi_level(-127) == 0);
}

static void test_adv_retry_delay(void)
{
    assert((ap_adv_retry_delay_ms(0) == 250) && "首次重试要快（断开后尽快重新可连）");
    assert(ap_adv_retry_delay_ms(1) == 500);
    assert(ap_adv_retry_delay_ms(2) == 1000);
    assert(ap_adv_retry_delay_ms(3) == 2000);
    assert(ap_adv_retry_delay_ms(4) == 4000);
    assert((ap_adv_retry_delay_ms(5) == 5000) && "封顶 5s");
    assert((ap_adv_retry_delay_ms(50) == 5000) && "封顶对任意次数都成立");
    assert((ap_adv_retry_delay_ms(-3) == 250) && "非法输入钳制到 0 档");
}

int main(void)
{
    test_rssi_level();
    test_adv_retry_delay();
    printf("== conn logic 测试全部通过 ==\n");
    return 0;
}
