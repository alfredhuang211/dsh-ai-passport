// main/ap_launcher_hook.c —— 见头文件说明。
//
// 设计要点（每条都对应一个容易踩的坑）：
//   · "运行在哪个槽"只能按**运行时**分区表判断。子固件运行时不使用自己的
//     partitions.csv（见项目 README「部署方式」），所以编译期常量/本地槽表
//     都是错的，esp_ota_get_running_partition() 才是唯一可信来源。
//   · 返回启动器只做一件事：把引导分区写回 factory，然后重启。不碰 ota 槽、
//     不擦数据分区，因此无论成功失败都不会留下"引导指向无效镜像"的砖化状态。
//   · standalone 模式（factory 就是自己）下必须 no-op：本地 partitions.csv 里
//     根本没有 otadata 分区，写入必然失败；把它当成"失败后重启"会让每一下
//     "长按确定"都变成一次重启，在开发机上很难与真实故障区分。
#include "ap_launcher_hook.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"

#include <stdint.h>

static const char *TAG = "launcher";

/** factory app 分区（启动器所在槽）。找不到返回 NULL。 */
static const esp_partition_t *find_factory_partition(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                    ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
}

bool ap_launcher_running_child(void)
{
    // running 由 IDF 按"本函数代码所在物理地址"反查运行时分区表得到。
    // 子槽运行时它指向 ota_x；standalone 运行时它指向 factory。
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *factory = find_factory_partition();
    if (!running || !factory) {
        // 没有 factory 分区 → 不存在"启动器槽"这种形态，按 standalone 处理。
        return false;
    }
    // 比物理地址而不是指针：两者都来自分区表条目，但不同 API 不保证返回
    // 同一个动态条目；地址相等才是"同一个槽"的可靠判据。
    return running->address != factory->address;
}

esp_err_t ap_launcher_return(void)
{
    if (!ap_launcher_running_child()) {
        // standalone/factory 形态：没有启动器可返回。
        // 安全 no-op —— 不写分区也不重启（否则开发机上每一次"长按确定"都会
        // 变成一次重启）。按键语义仍由调用方消费，界面行为与改造前一致。
        ESP_LOGW(TAG, "factory/standalone 模式：没有可返回的启动器，忽略返回请求");
        return ESP_ERR_NOT_SUPPORTED;
    }

    const esp_partition_t *factory = find_factory_partition();
    if (!factory) {
        // ap_launcher_running_child() 为真时不可能走到这里；防御性保留。
        ESP_LOGE(TAG, "运行时分区表里没有 factory 分区，无法返回启动器");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "返回启动器：引导分区 -> factory（0x%lx，%lu KB），重启",
             (unsigned long)factory->address, (unsigned long)(factory->size / 1024));

    // esp_ota_set_boot_partition(factory) 的语义正是"下次启动 factory"：
    //   内部先校验 factory 里的镜像，再擦掉 otadata（清掉指向 ota 槽的选择）。
    //   factory 镜像无效 / 分区表里没有 otadata 时返回错误，且**不改动 otadata**。
    const esp_err_t err = esp_ota_set_boot_partition(factory);
    if (err != ESP_OK) {
        // 不能因为写引导失败就停在这里（用户按键后无反应）。直接重启：
        // 单次会话约定下由启动器 / bootloader rollback 兜底决定下一次启动，
        // 最坏结果也只是再次进入本固件，用户可以再试或断电。
        ESP_LOGE(TAG, "写引导分区失败：%s（未改动分区，直接重启）", esp_err_to_name(err));
    }

    esp_restart();   // 正常路径不返回
    return err;      // 理论不可达；给调用方一个确定的返回值
}
