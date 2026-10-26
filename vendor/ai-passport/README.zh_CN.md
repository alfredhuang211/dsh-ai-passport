<p align="right">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

# DSH Passport

把 [FoloToy AI Passport](https://ai-passport.folotoy.cn/) 变成
[DeepSeek Harness](https://github.com/deepseek-ai)（DSH）的**随身任务终端**。

在设备上就能管理电脑上跑的 AI 任务：

| 你能做的事 | 说明 |
| --- | --- |
| 看任务台 | 浏览、切换、新建 DSH 任务，运行中的任务显示当前步骤和正在执行的操作 |
| 收完成提醒 | 任务结束弹横幅 + 提示音，成功、出错、中断三种音效；未读任务带红点 |
| 批工具调用 | 电脑上的 AI 要动文件或跑命令时，设备弹出授权页，按上下键选"允许/拒绝"，倒计时归零自动拒绝 |
| 说一句话下指令 | 长按录音，说完松手，中文识别成文字后回填到电脑输入框 |
| 查余额 | 长按下键看 DeepSeek 余额概览 |

连接方式是**蓝牙近场直连电脑**：不需要 Wi-Fi、不需要配网、不需要输密码。首次配对时设备屏幕显示 6 位配对码，在电脑上输一次即可，配对码每次上电都会换。

## 需要配套的电脑端插件

这个固件只负责设备那一半，完整的玩法需要电脑端插件：

**电脑端插件：[`slinxiaosun-blip/dsh-ai-passport-plugin`](https://github.com/slinxiaosun-blip/dsh-ai-passport-plugin)**

它负责蓝牙链路、任务桥接、语音识别与桌面挂件。安装方式：把该仓库 clone 到本地，
运行其中的 `npm run install-plugin`（macOS / Windows 同一命令，安装器自动适配两个平台），
详见该仓库 README 的「快速开始」。

固件与插件之间的通信协议版本必须匹配（当前协议 v1）。固件**不区分主机平台**：
同一份镜像对 macOS 与 Windows 的 DSH 客户端通用，一次烧录即可（设备只做 BLE 外设，
分片按协商 MTU 动态计算，配对走应用层配对码，均与主机操作系统无关）。

## 下载固件（不用自己编译）

最新固件在 [Releases](https://github.com/slinxiaosun-blip/ai-passport-dsh/releases/latest) 页面，
下载 `FoloToy-AI-Passport-full.bin` 即可，它可以从 `0x0` 一次性刷入：

```bash
# macOS / Linux（串口形如 /dev/cu.usbserial-XXXX 或 /dev/cu.wchusbserial-XXXX）
esptool.py -p /dev/cu.usbserial-XXXX write_flash 0x0 FoloToy-AI-Passport-full.bin

# Windows（串口形如 COM5，设备管理器里可查）
esptool.py -p COM5 write_flash 0x0 FoloToy-AI-Passport-full.bin
```

刷写会覆盖设备原有固件，且不保证保留已有设置。**刷之前先记下当前固件版本**，
方便回退。恢复官方固件用 [AI Passport 刷机工具](https://ai-passport.folotoy.cn/tools/web-flasher/)。

## 自己编译

需要 ESP-IDF **v5.5.3**（装在仓库外、路径不含空格）：

```bash
# 仓库检查 + 逻辑测试（秒级，不需要硬件）
./tools/validate.sh --static

# 完整构建，产出可从 0x0 一次性刷写的合并镜像（分钟级）
./tools/validate.sh --firmware
```

产物：`build/FoloToy-AI-Passport-full.bin`

刷写（会覆盖设备原有固件）：

```bash
# macOS / Linux
esptool.py -p /dev/cu.usbserial-XXXX write_flash 0x0 build/FoloToy-AI-Passport-full.bin

# Windows
esptool.py -p COM5 write_flash 0x0 build/FoloToy-AI-Passport-full.bin
```

想恢复官方固件，用 [AI Passport 刷机工具](https://ai-passport.folotoy.cn/tools/web-flasher/)。构建环境准备、常见偏差处理、插件安装与语音模型启用，见插件仓库的
[docs/03-构建与烧录.md](https://github.com/slinxiaosun-blip/dsh-ai-passport-plugin/blob/main/docs/03-构建与烧录.md)。

## 版本

固件版本由 `version.txt` 提供基底（当前 `1.1.0`），构建时自动追加 git 短哈希，
例如 `1.1.0+g1a2b3c4-dirty`。这个串会随握手上报给电脑端，挂件信息格里显示的就是设备真实版本。

- `1.1.0` —— 与插件 1.1.0 同步发布（插件侧适配 macOS / Windows 双 DSH 客户端）；
  固件行为与 `1.0.0` 完全一致，此版本仅做版本对齐，一次烧录两端通用。
- `1.0.0` —— 首个完整版本（任务台/审批/余额/语音/配对）。

## 来源与许可

本仓库基于上游 [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport)
`main @ 0b9e4c8`（MIT）二次开发（作者 [slinxiaosun-blip](https://github.com/slinxiaosun-blip)），
全部改动集中在 `feature/dsh-passport` 分支：

- **新增**：`main/app_*.c/.h` 设备应用（蓝牙链路、任务台、审批、问答、语音、配对、设置）
- **侵入式改动**：`components/bsp` 四处、`main/CMakeLists.txt`、`main/main.c`、
  `sdkconfig.defaults`、`tests/bsp_stubs`、`tools/`
- 上游 `main` 分支保持与 `FoloToy/ai-passport` 同步，不承载本项目改动

遵循上游 MIT 许可，原始版权归 FoloToy 所有。
