# @alfredhuang211/dsh-ai-passport

把 [FoloToy AI Passport](https://ai-passport.folotoy.cn/)（ESP32-C3 掌上设备）接进 DeepSeek Harness 的插件：在设备上新建与选择任务、看执行状态、收完成提醒、做任务审批、查 DeepSeek 余额，并用语音下指令。设备与电脑之间走 BLE，不需要 Wi-Fi。

固件源码与协议说明在同一个仓库的其它目录里，本目录只是 npm 包这一半。

## 安装

```bash
dsh plugin --profile desktop add @alfredhuang211/dsh-ai-passport
```

### 蓝牙支持需要单独装一个原生依赖

插件本体零依赖，`dsh plugin add` 一次就能装好并启用。

蓝牙功能依赖 `@stoprocent/noble`。它被声明为**可选 peer 依赖**，装插件时不会被拉进来——
这样安装不会因 pnpm 拦截原生构建脚本而失败，也不用为一次安装去改 profile 的 `allowBuilds`。

需要蓝牙时，把它装进同一个 profile：

```bash
dsh plugin --profile desktop add @stoprocent/noble
```

它自带各平台预编译产物，通常不需要本地编译；若 pnpm 提示 `ERR_PNPM_IGNORED_BUILDS`，
把 profile 的 `pnpm-workspace.yaml` 里对应项改为 `true` 后重跑一次即可。请把这次批准理解为
「允许该包的安装脚本在本机、且不在 agent 沙箱内执行」，只对信得过的源码这样做。

没装 noble 时插件照常加载，只有蓝牙相关功能不可用，面板会显示为未加载状态。

装完重启 DSH，设置页会出现本插件；设备侧需要先刷固件并完成配对。

## 它做什么

| 能力 | 说明 |
|---|---|
| BLE 联机 | 后台桥进程管理蓝牙连接，避免原生依赖的 ABI 问题拖垮宿主 |
| 任务 | 在设备上新建与选择 DSH 任务 |
| 状态与提醒 | 执行状态推送到设备，完成时提醒 |
| 审批 | 把工具的审批请求送到设备上确认 |
| 余额 | 查 DeepSeek 账户余额 |
| 语音 | 设备录音上行为输入文本 |

`autoConnect` 默认为 `false`：插件不会在宿主启动的关键路径上拉起蓝牙桥进程，需要你在面板里点「扫描并连接」。

## 已知限制

- 需要本机有可用的蓝牙适配器，且系统已授予权限。
- 设备端固件版本要与插件的协议版本匹配。
- 目前只发布到 npm；`dsh plugin` 无法管理 desktop profile 之外的若干操作，开发时以本地链接为主。

## 许可

MIT，见 [LICENSE](LICENSE)。
