// main/ap_launcher_hook.h —— 多应用启动器（MetaPass2 / meta-pass legacy）子固件适配钩子。
//
// 本固件有两种运行形态，代码必须同时成立：
//
//   1) 子槽形态（C3 正式部署）：被启动器从某个 OTA 子槽（ota_0/1/2）拉起，
//      运行时不使用本仓库 partitions.csv 生成的分区表，而是**启动器烧在 0x8000
//      的分区表**；此时"返回启动器"= 把引导分区指回 factory 后重启。
//   2) standalone 形态（开发/自烧）：从 factory 启动，分区表就是本仓库的
//      partitions.csv（只有 factory 一个 app 分区）。此时没有启动器可返回，
//      返回请求必须**安全降级**，绝不能写坏引导分区。
//
// ★ 为什么不用编译期槽位常量：
//   子固件放哪个槽由启动器决定（meta-pass legacy 与 MetaPass2 v2.x 的槽表并不
//   相同），写死 0x180000 / 0x1D6000 之类偏移在换布局后只会静默失效，甚至写错
//   位置。这里全部走运行时 API（esp_ota_* / esp_partition_*），启动器布局变化
//   不需要改本模块，也不需要在编译期知道槽大小。
//
// ★ 本模块不做、也禁止做的事：
//   · 不调用任何 OTA 自我确认接口（esp_ota_mark_* 系列；单次会话约定：子固件
//     不确认自己，任何重启由启动器 / bootloader rollback 决定下一次启动哪个槽）；
//   · 不写、不擦任何 OTA 槽或数据分区；
//   · 唯一的持久副作用是返回时把 otadata 指回 factory（esp_ota_set_boot_partition），
//     失败时连这一步都不做，只记日志后重启。
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 当前是否运行在启动器的 OTA 子槽中。
 *
 * 判据：esp_ota_get_running_partition()（按"本函数代码所在的物理 Flash 地址"
 * 在运行时分区表里定位分区）与 factory 分区是否为同一个。
 *
 * 注意这是**运行形态**而不是"是否由启动器安装"：开发期手工把镜像写到某个
 * 槽、或用启动器安装，两种方式此函数都返回 true；烧到 factory 则返回 false。
 * 找不到 factory 分区（纯 OTA 布局）时也返回 false —— 那种布局没有"启动器槽"
 * 可以返回。
 */
bool ap_launcher_running_child(void);

/**
 * 返回启动器。
 *
 * · 子槽形态：esp_ota_set_boot_partition(factory) → esp_restart()，正常路径不返回；
 * · standalone/factory 形态：**安全 no-op**（不改分区、不重启），返回
 *   ESP_ERR_NOT_SUPPORTED，行为与改造前一致；
 * · 子槽形态但引导分区写入失败（factory 镜像无效 / 分区表无 otadata 等）：
 *   记日志、**不做任何额外写操作**，随后 esp_restart() —— 单次会话约定下，
 *   重启由启动器 / bootloader rollback 兜底，最坏结果也只是再次进入本固件。
 *
 * 可在任意任务上下文调用（app 任务 / 按键路径）；内部不访问 LVGL、不做长阻塞。
 * 调用方不需要预先判断运行形态，本函数内部会判断。
 *
 * @return ESP_OK 仅当 esp_restart() 意外返回（理论上不会发生）；no-op 或写引导
 *         分区失败时返回对应错误码。调用方通常可以直接忽略返回值。
 */
esp_err_t ap_launcher_return(void);

#ifdef __cplusplus
}
#endif
