// main/app_conn_logic.c —— 连接呈现与重连退避的纯逻辑。语义见 app_conn_logic.h。
#include "app_conn_logic.h"

int ap_rssi_level(int rssi)
{
    // 门槛取经验值：-60 以上贴身/同桌，-70~-80 正常室内，-90 边缘。
    // 0 格（红）表示"能连但随时会断"，比隐藏问题诚实。
    if (rssi >= -60) return 4;
    if (rssi >= -70) return 3;
    if (rssi >= -80) return 2;
    if (rssi >= -90) return 1;
    return 0;
}

uint32_t ap_adv_retry_delay_ms(int attempt)
{
    if (attempt < 0) attempt = 0;
    // 250 → 500 → 1000 → 2000 → 4000，封顶 5000ms：
    // 首要目标是快速自愈（断开后几百毫秒内重新可连），持续失败也只占极低功耗。
    if (attempt >= 5) return 5000;
    return 250u << attempt;
}
