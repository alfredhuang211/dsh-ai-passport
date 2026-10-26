# DSH AI Passport

把 [FoloToy AI Passport](https://ai-passport.folotoy.cn/)（ESP32-C3 掌上设备）做成
[DeepSeek Harness](https://github.com/deepseek-ai)（DSH）的**随身任务终端**：

在设备上新建/选择 DSH 任务、看执行状态、收完成提醒、做任务审批、查 DeepSeek 余额、
用语音下指令。设备与电脑（macOS / Windows 的 DSH 客户端）之间走 BLE 蓝牙无线联机，无需 Wi-Fi。

```
┌─ AI Passport（设备） ─┐   BLE 5（GATT，自定义帧 + JSON）   ┌─ Mac/Win + DSH ───┐
│  ESP32-C3 固件        │ ◄──────────────────────────────► │  本插件（npm 包）   │
│  app_*.c / LVGL UI   │      协议 v2 · 双端常量同步        │  ble/bridge/panel  │
└──────────────────────┘                                   └───────────────────┘
```

## 单仓库结构

插件与固件源码在**同一个仓库**里，统一构建、同版本发版：

| 目录 | 内容 |
| --- | --- |
| `packages/dsh-ai-passport/` | DSH 插件（npm 包：BLE 桥、协议、面板、客户端 bundle） |
| `vendor/ai-passport/` | ESP32-C3 固件源码（`app_*.c` / LVGL UI / BSP / 测试），基于上游 FoloToy/ai-passport 二次开发 |
| `docs/` | 跨端设计、构建烧录、验收文档 |
| `tools/` | 固件构建、烧录、字体子集、音效素材生成脚本 |

固件的环境、编译、烧录见 [docs/03-构建与烧录.md](docs/03-构建与烧录.md)：

```bash
source ~/esp/esp-idf-v5.5.3/export.sh
cd vendor/ai-passport && ./tools/validate.sh          # 静态门禁 + host 测试 + 固件构建
```

历史固件仓库 [`ai-passport-dsh`](https://github.com/slinxiaosun-blip/ai-passport-dsh) 仍保留
（用于对照上游与历史 Release）。本仓 `vendor/ai-passport` 是最新源码快照（含协议 v2 / 多会话 / 2.0.0 改造）。
不打算自己编译固件的话，可直接用该仓库的 [Releases](https://github.com/slinxiaosun-blip/ai-passport-dsh/releases/latest)
镜像（注意：Release 可能落后于本仓源码，以本仓 `vendor/ai-passport/version.txt` 为准）。

## 功能

| 能力 | 说明 |
| --- | --- |
| 多会话菜单 | 设备显示最多 8 个会话（绿=运行/黄=等待/红=出错或中断/灰=空闲），上下选择、确定进入；会话内看 step/工具/摘要，未读点区分 |
| 完成提醒 | 非模态横幅 + BP 机提示音（出错加低尾音、中断三连哔）+ 未读点 |
| 任务审批 | `approval/request` 瀑布拦截，弹窗显示**所属会话名**，设备端允许/拒绝（高风险工具默认停在拒绝），双咳提示音 |
| 配对 | 6 位配对码（每次上电换），NVS 密钥 + 常量时间比较 + 限流 |
| DeepSeek 余额 | 大字显示，取不到时明确报错，不显示伪造的 0 |
| 语音输入 | 会话内按住确定说话 → BLE 上行 ADPCM → 本地 SenseVoice 离线识别 → **定向发回该会话**。**单次最长 30 秒** |
| 设置侧栏 | DSH 设置里的独立「AI Passport」页：连接/配对/显示/语音/会话接入/关于（`settings.section`） |
| 会话旁指示 | 会话标题旁三态 chip：未连接 / 设备已连接 / 本会话已在设备上（`conversation.session.header.utilities`） |
| Web 面板 | 本地控制面板（回环限定 + SSE 实时推送） |

### 语音输入的时长限制

| 项 | 值 | 说明 |
| --- | --- | --- |
| **单次录音上限** | **30 秒** | 说到 30 秒设备**自动结束**，不需要松手，随后送去识别 |
| 录音结束条件 | 仅两个 | 「松开按键」或「触达 30 秒上限」 |
| 说话中途停顿 | **不会**被切断 | 没有"静音自动结束"；停顿、思考、句间留白原样录进去 |
| 主机侧转写上限 | 默认 30 秒 | `voice.maxSeconds`，**只能调短**：超限截断尾部再识别，不丢弃整段录音 |
| 低于 0.5 秒 | 丢弃 | 按压误碰，不进识别、也不报错 |

要点：

- **上限 30 秒是硬限制，写在固件里**（`AP_AUDIO_MAX_SECONDS`）。想调大得改固件重新编译烧录；
  插件侧的 `voice.maxSeconds` 调大不会让设备录更久。
- 说不到上限就松手 = 按实际时长结束，说满 30 秒会自动停。两种都正常识别。
- 需要说更长的内容：分几次按着说，识别结果逐条进输入框；或在电脑端直接打字。
- 设备说话时**中途停顿不会被切断**，可以自然停顿后再继续。

> 固件 1.2.0 起上限由 15 秒放宽到 30 秒，并取消了那之前的"静音 1.2 秒自动结束"。
> 若你的插件低于 1.2.0，超过 10 秒的录音会**静默失败**（设备自己停了但不出结果也不报错），
> 详见 [docs/02 的 2026-10-10 返工记录](docs/02-进展与下一步.md)。

## 快速开始

前置：DSH 桌面版（macOS 或 Windows）、Node ≥ 20。BLE 走 noble 原生后端：
macOS 限 Apple Silicon；Windows 需 Windows 10 1703+（x64，noble 有 win32-x64 预编译；
ARM64 需源码编译，见 [docs/08](docs/08-Windows反馈清单.md)）。

```bash
# 0) clone 本仓库（插件 + 固件源码都在里面）
git clone https://github.com/alfredhuang211/dsh-ai-passport.git
cd dsh-ai-passport-plugin

# 1) 安装依赖
npm install

# 2) 把插件装进 DSH profile（macOS / Windows 同一命令；链接方式，源码即生效代码）
npm run install-plugin                 # 默认 desktop
npm run install-plugin -- web desktop  # 可指定多个 profile

# 3) 无硬件联调（虚拟设备会应答握手、心跳、任务列表、余额）
npm test
```

> Windows 也可用等价入口：`powershell -ExecutionPolicy Bypass -File tools\install-plugin-to-dsh.ps1`。
>
> 脚本对每个 profile 做 `node_modules/dsh-ai-passport -> 本仓库源码` 的链接
> （macOS 用符号链接；Windows 用目录 junction，**无需**管理员权限；被机器策略禁止时
> 自动降级为复制并明确提示），改源码即时生效。`desktop` profile 由 Electron 独占管理
> （`dsh plugin` CLI 拒绝操作），因此不走 CLI，手工链接是唯一可靠的开发安装方式。

装好后打开控制面板（DSH 内置 webserver）：

```
http://127.0.0.1:19387/dsh-passport
```

常用配置（DSH 设置界面或 `cordis.patch.yml`）：

| 配置 | 默认 | 说明 |
| --- | --- | --- |
| `transport` | `bridge` | `bridge`（子进程）/ `mock`（无硬件联调）/ `noble`（进程内） |
| `autoConnect` | `true` | 插件加载后自动扫描连接 |
| `tasks.maskTitles` | `false` | 设备上只显示"任务 xxxx"，适合公共场合 |
| `voice.directSend` | `true` | 设备识别结果卡「双击确定」= 整段替换草稿并发送 |
| `voice.maxSeconds` | `30` | 单次转写长度上限（秒），超限截断；固件端自律同样是 30s |

完整配置表与语音识别（SenseVoice 本地模型）启用方式见
[docs/03-构建与烧录.md](docs/03-构建与烧录.md) 与 [docs/04-桌面客户端集成.md](docs/04-桌面客户端集成.md)。

## 版本与兼容性

当前实测环境（2026-10-07）：

| 组件 | 版本 | 说明 |
| --- | --- | --- |
| DSH 桌面版 | `0.2.1-rc.3` | 实测基线（`settings.section` / 会话作用域槽位）；macOS 与 Windows 同一份插件代码 |
| dsh CLI | `0.1.7-rc.2` | `dsh --version` 实测 |
| 插件（本仓） | `2.0.0` | `packages/dsh-ai-passport/package.json` |
| 设备固件 | `2.0.0` + git 短哈希 | 固件仓 `version.txt`，构建期自动追加 `git describe` |
| BLE 协议 | `v2` | 双端常量 `PROTOCOL_VERSION` ↔ `AP_PROTOCOL_VERSION`，取较小值运行；旧固件 v1 走兼容路径一个版本周期 |
| 固件基线 | `main @ 0b9e4c8` | 上游 [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) |
| Node | ≥ 20 | 开发机 22.22.2；DSH 桌面版内置 Node 24.21.0 |
| 操作系统 | macOS（Apple Silicon）/ Windows 10 1703+ | BLE 后端：CoreBluetooth / WinRT（noble）；固件一次烧录两端通用 |

版本记录：

- `2.0.0` —— **多会话 + 设备菜单 + 设置页重构**：协议升级 v2（`session.list/detail/done/select`、`settings.set`、`sound.test`），
  设备主页改为会话列表（状态色条/未读点/进入会话）、会话内按住说话定向发送、审批弹窗显示所属会话；
  桌面端删除输入框旁挂件（修复官方语音插件被挤占），改为设置侧栏独立页 + 会话旁三态指示；
  提示音升级为「QQ 双咳/单咳采样 + BP 机完成音/出错低尾音/中断三连哔」；固件 `2.0.0` 同步。
  旧固件按 v1 兼容（`task.state`）运行。
- `1.2.0` —— **修复长录音静默失败**：任何超过 10 秒的录音都会在设备自动停止后
  "什么也不出"且无任何报错（主机侧会话存活期窗口写死 10s，短于录音上限）；
  现改为由录音上限派生。同时让 `voice.maxSeconds` 真正生效（此前是死配置），
  超限截断而非丢弃。配合固件 `1.2.1`（录音上限 30s、取消静音自动结束）。
- `1.1.0` —— 插件安装与运行自动适配 macOS / Windows 两个 DSH 客户端
  （安装器 junction/symlink 自适应、桥进程 Node 探测平台化、报错指引分平台）；
  设备固件行为不变、一次烧录两端通用，`1.1.0` 仅是版本对齐。
- `1.0.0` —— 阶段 A/B/C 全功能首个版本（任务台/审批/余额/语音/配对）。

> 固件源码已并入本仓 `vendor/ai-passport/`；历史 Release 见 [ai-passport-dsh](https://github.com/slinxiaosun-blip/ai-passport-dsh/releases)：
> `1.2.1`（录音上限 15s → 30s，取消静音 1.2s 自动结束，说话中途的停顿不再被切断）。
> 协议 v2 起双端需同一代；旧固件 v1 仍可被新插件兼容一个版本周期，反之不保证。

> 插件依赖 DSH `0.2.0-rc.x` 的 `ctx.webServer` / `speechToText` / `deepseekAccount`
> 等宿主 API。DSH 升级前先跑 `npm test`（mock 联调）+ [docs/07](docs/07-设备端验收用例.md)
> 真机验收，通过再升级。

## 目录结构

```
├── packages/dsh-ai-passport/   DSH 插件（npm 包，独立可 link 安装）
│   ├── lib/ble/                transport 抽象 + noble / 桥进程 / mock 三驱动
│   ├── lib/protocol/           帧编解码（协议 v1）
│   ├── lib/bridge/             session / watch / approval / balance / voice / pairing
│   ├── lib/panel/              Web 控制面板
│   ├── lib/client.js           桌面客户端输入框挂件
│   └── cordis.patch.yml         DSH 配置树补丁
├── docs/                       跨端文档（见下方索引）
├── tools/                      主机侧工具（插件安装 / 截图 / 布局校验 / 字体子集化）
├── assets/fonts/               字体源文件（含 LICENSE）
└── vendor/ai-passport/         固件仓库（独立 git，clone 进来，不入库）
```

## 文档索引

| 文档 | 内容 |
| --- | --- |
| [01-方案设计](docs/01-方案设计.md) | 系统架构、BLE 协议、设备端交互、语音链路 |
| [02-进展与下一步](docs/02-进展与下一步.md) | 开发日志：阶段 A/B/C 全记录 |
| [03-构建与烧录](docs/03-构建与烧录.md) | 环境、固件编译、烧录、插件安装、版本号机制 |
| [04-桌面客户端集成](docs/04-桌面客户端集成.md) | 输入框挂件形态、语音回填、桌面端行为 |
| [05-语音发送-设计](docs/05-语音发送-设计.md) | 语音链路设计 |
| [06-设备端问答与语音直发-设计](docs/06-设备端问答与语音直发-设计.md) | 设备端问答与语音直发 |
| [07-设备端验收用例](docs/07-设备端验收用例.md) | 逐项验收表与真机实测记录 |
| [08-Windows反馈清单](docs/08-Windows反馈清单.md) | Windows 客户端适配说明、已知边界、用户反馈清单 |
| [09-多会话与设备菜单-设计](docs/09-多会话与设备菜单-设计.md) | 协议 v2、多会话列表、设备菜单层级、设置侧栏独立页（设计稿 + §12 实现与验收记录） |

## 开发须知

- **协议常量双端同步**：`packages/dsh-ai-passport/lib/protocol/constants.js` 的
  `PROTOCOL_VERSION` 与固件仓 `main/app_proto.h` 的 `AP_PROTOCOL_VERSION` 手工保持一致，
  改协议必须两侧同时 bump，并跑双端 parity 测试。
- **测试**：`npm test`（顶层 workspace 转发；固件侧逻辑测试在固件仓
  `./tools/validate.sh --static`）。
- **版本**：固件 `version.txt` + 构建期 `git describe`；插件版本在本仓 `package.json`。

## License

MIT © 2026 slinxiaosun-blip

本仓库（DSH 插件、跨端文档与主机侧工具）是**独立原创作品**，版权归作者本人所有。

设备端固件是另一回事：它是 [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport)
（MIT，版权归 FoloToy）的二次开发作品，遵循其原有许可，版权行见固件仓库的 `LICENSE`。
两个仓库各自独立授权，互不派生。
