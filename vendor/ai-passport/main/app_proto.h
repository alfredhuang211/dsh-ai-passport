// main/app_proto.h —— AI Passport ⇄ DSH 联机协议的共享常量。
//
// ★ 这个文件是 packages/dsh-ai-passport/lib/protocol/constants.js 的 C 侧镜像。
//   两端**必须逐字段一致**：帧头布局、通道号、标志位、消息类型字符串、
//   分片上限、心跳周期。改任何一处都要同步改另一处。
//
//   为什么不做成生成式（从一份定义生成两边）：C 与 JS 的构建链完全不同，
//   引入代码生成会让"改一个常量"变成"跑一个生成器"，在只有两个人维护的小项目里
//   反而更容易忘记重跑。改为在 host 侧加一条自动比对检查（见
//   packages/dsh-ai-passport/test/protocol-parity.test.js），并在下面逐条注明来源。
//
//   改常量时的检查顺序：
//     1. 改这里 + 改 constants.js
//     2. 跑 host 侧 parity 测试
//     3. 若改了帧头或分片上限，设备与旧主机将无法互通 → 必须同时提升 PROTOCOL_VERSION
#pragma once

#include <stdbool.h>
#include <stdint.h>

// ── 版本 ───────────────────────────────────────────────────────────────────
// 与 constants.js 的 PROTOCOL_VERSION 对应。两端取较小值运行；
// 不一致时设备屏幕必须显式提示，而不是静默失败（静默失败最难查）。
#define AP_PROTOCOL_VERSION 2

/** 固件版本，随 hello 上报给主机（用于"设备固件太旧"这类诊断）。 */
// 固件版本：**构建时注入**（见 main/CMakeLists.txt）。
// 以前这里是手写常量，结果挂件上永远显示同一个旧版本号（用户反馈"版本好像不对、
// 没有根据设备来获取"）—— 手写的东西没人记得每次改。
// 注入串形如 "0.2.0+g1ccb7e1" 或 "0.2.0+g1ccb7e1-dirty"，一眼能对上构建产物。
#ifndef AP_FIRMWARE_VERSION
#define AP_FIRMWARE_VERSION "0.2.0-unknown"
#endif

// ── GATT 标识 ──────────────────────────────────────────────────────────────
// UUID 由 constants.js 的 UUID 表镜像而来：0000a900-5041-5353-504f-5254a900xxxx
//
// ★ 字节顺序是个容易写错的地方：NimBLE 的 BLE_UUID128_INIT 按**小端**给出 16 字节，
//   而字符串是**大端**表示。所以要把字符串按字节反着写：
//     字符串 00 00 a9 00 | 50 41 53 53 | 50 4f 52 54 | a9 00 00 01
//     字节序 01 00 00 a9 | 54 52 4f 50 | 53 53 41 50 | 00 a9 00 00
//   前 12 字节（后三段反序）对四条 characteristic 完全相同，只有最后一组
//   （服务/特征值编号）不同 —— 因此用 AP_UUID_TAIL 抽出公共部分，避免复制出错。
// 16 字节 UUID 的**完整**线上字节序（不再是"尾巴"，见下方说明）。
//
// ★ 这里踩过一个很隐蔽的坑，改之前务必读完：
//
// 早期宏参数写成 (0x54,0x52,0x4f,0x50,0x53,0x53,0x41,0x50,0x00,0xa9,0x00,0x00, 0x00, 0xa9)，
// NimBLE 的 BLE_UUID128_INIT 会把参数**反向**展开，于是线上字节是
//     01 00 00 A9 50 53 4F 52 4F 54 53 53 41 50 00 A9
// 按标准记法还原成 `0000a900-0000-a900-5041-5353-504f5254` ——
// 形如 xxxxxxxx-0000-xxxx-xxxx-xxxxxxxxxxxx，正落在蓝牙规范给
// **16 位 UUID 缩写**保留的占位模式里。macOS 的 CBUUID 对该模式强制校验，
// 直接抛 NSInternalInconsistencyException:
//     "String 0000A900-0000-A900-5041-5353-504F5254 does not represent a valid UUID"
// 而且**不是返回错误，是终止整个进程**。排查时表现为 node 被 CoreBluetooth 杀掉，
// 极易误判成固件或原生模块崩溃。
//
// 修法：把前 4 字节改成非零的 F0 A9 00 01，第三段于是变成 4f52 而非 0000：
//     线上 16 字节 = 01 00 A9 F0 50 53 4F 52 4F 54 53 53 41 50 00 A9
//     标准记法     = f0a90001-5053-4f52-4f54-5353-4150-00a9
// 后 12 字节保持原样（本项目自己的可读标签）。主机侧 constants.js 的 UUID 必须与之逐字一致，
// 由 protocol-parity 测试卡住。
//
// 注意 BLE_UUID128_INIT 的第一个参数仍是"低 12 字节在前"的顺序，所以这里照旧写 12 字节，
// 前 4 字节由调用处的 (0x01, 0xf0, 0xa9, 0x00) 提供 —— 见 app_link.c。
#define AP_UUID_TAIL 0x50, 0x53, 0x4F, 0x52, 0x4F, 0x54, 0x53, 0x53, 0x41, 0x50, 0x00, 0xA9

// 完整 16 字节 UUID。
//
// ★ 两个宏别混用（编译报错会指向 ble_uuid.h，很容易看歪）：
//     BLE_UUID128_DECLARE(...)  → 带字段名的初始化列表，用于**结构体字段**
//     BLE_UUID128_INIT(...)     → 赋值表达式，用于 **ble_uuid128_t 变量**
//
// 两个宏都接受 16 个字节，且**都按"线上顺序"书写，由宏负责反转**。
// 因此下面的参数顺序与线上字节逐字对应，可以直接肉眼核对：
//
//     01 00 A9 F0 | 50 53 4F 52 | 4F 54 53 53 | 41 50 00 A9
//
// 标准记法 = f0a90001-5053-4f52-4f54-5353-4150-00a9
//
// 为什么不再用「12 字节尾巴 + 补位参数」的写法：那种写法里"参数怎么写"与
// "线上是什么"之间隔了一层反序，上一版正是在这里把 UUID 写错并长期没被发现
// （表现为 macOS 的 CBUUID 因保留占位模式直接终止进程）。写全 16 字节后不再需要心算反序。
//
// 与主机侧 packages/dsh-ai-passport/lib/protocol/constants.js 必须逐字节一致，
// 由 protocol-parity 测试卡住。
#define AP_UUID_SERVICE \
    BLE_UUID128_DECLARE(0x01, 0x00, 0xA9, 0xF0, 0x50, 0x53, 0x4F, 0x52, 0x4F, 0x54, 0x53, 0x53, 0x41, 0x50, 0x00, 0xA9)
#define AP_UUID_CHAR_RX \
    BLE_UUID128_DECLARE(0x02, 0x00, 0xA9, 0xF0, 0x50, 0x53, 0x4F, 0x52, 0x4F, 0x54, 0x53, 0x53, 0x41, 0x50, 0x00, 0xA9)
#define AP_UUID_CHAR_TX \
    BLE_UUID128_DECLARE(0x03, 0x00, 0xA9, 0xF0, 0x50, 0x53, 0x4F, 0x52, 0x4F, 0x54, 0x53, 0x53, 0x41, 0x50, 0x00, 0xA9)
#define AP_UUID_CHAR_VOICE \
    BLE_UUID128_DECLARE(0x04, 0x00, 0xA9, 0xF0, 0x50, 0x53, 0x4F, 0x52, 0x4F, 0x54, 0x53, 0x53, 0x41, 0x50, 0x00, 0xA9)
#define AP_UUID_CHAR_CTRL \
    BLE_UUID128_DECLARE(0x05, 0x00, 0xA9, 0xF0, 0x50, 0x53, 0x4F, 0x52, 0x4F, 0x54, 0x53, 0x53, 0x41, 0x50, 0x00, 0xA9)

// 广播里用的服务 UUID 是**变量**，这条路径要用 INIT 版本。
#define AP_UUID_SERVICE_VAR \
    BLE_UUID128_INIT(0x01, 0x00, 0xA9, 0xF0, 0x50, 0x53, 0x4F, 0x52, 0x4F, 0x54, 0x53, 0x53, 0x41, 0x50, 0x00, 0xA9)
#define AP_SERVICE_UUID 0xa900   // 主服务（Primary Service）
#define AP_CHR_RX_UUID 0xa901    // Mac → 设备（Write Without Response）
#define AP_CHR_TX_UUID 0xa902    // 设备 → Mac（Notify，JSON 控制）
#define AP_CHR_VOICE_UUID 0xa903 // 设备 → Mac（Notify，音频）
#define AP_CHR_CTRL_UUID 0xa904  // 设备 → Mac（Read，能力/版本/电量）

// GAP 设备名（连接后可读，也是完整的设备标识）。
#define AP_DEVICE_NAME "FoloPassport-DSH"
// ★ 广播包里用的**短名字**。
//
// 字节账（广播包只有 31 字节）：
//     Flags(AD 头 2 + 1)                 =  3
//     128 位服务 UUID(AD 头 2 + 16)       = 18
//     剩余                                = 10 → 名字最多 8 字符（AD 头 2 + 8）
// 因此这里必须 ≤ 8 字符。
//
// 为什么名字要放进**广播包**而不是只放 scan response：
// macOS 不保证把 scan response 合并进 advertisementData 交给 CoreBluetooth，
// 那样 noble 侧就只看得到 UUID、看不到名字，按名字过滤的客户端会"扫不到设备"
// （实测就是这样：主机列表里找不到它）。放广播包里最稳。
//
// 唯一性由 128 位服务 UUID 保证，名字只是给人看的。完整名字仍通过 GAP 设备名暴露。
#define AP_DEVICE_NAME_SHORT "Folo-PSP"

// 厂商标识：写进广播包的 Manufacturer Specific Data。
// 用途是给主机一个**与名字无关**的识别依据 —— macOS 不保证合并 scan response，
// 名字可能为空；而 128 位 UUID 在有些扫描路径里也不一定可见。
// 注意：第一段 ID 必须是 0xFFFF（见 AP_MANUFACTURER_ID 的说明），
// 后面 4 字节才是我们自己的载荷标记，内容为 "FAP1"。
#define AP_ADV_MFG_LEN 4
#define AP_ADV_MFG_TAG "FAP1"
// 兼容旧的裸广播名（刷过官方固件时用），Mac 侧两者都认。
#define AP_DEVICE_NAME_LEGACY "FoloPassport"
// Manufacturer Data 里的公司标识（自定义，仅用于识别本协议）。
#define AP_MANUFACTURER_ID 0xffff

// ── 帧格式 ─────────────────────────────────────────────────────────────────
// 每片 = 4 字节头 + 载荷（见 constants.js 顶部注释）
//   byte 0  version(高 4 位) | flags(低 4 位)
//   byte 1  channel
//   byte 2  msgId 高 8 位
//   byte 3  msgId 低 4 位(高 4 位) | seq 低 4 位
#define AP_HEADER_BYTES 4

// 通道号（constants.js 的 CHANNEL）
#define AP_CH_CONTROL 0
#define AP_CH_VOICE 1
#define AP_CH_HEARTBEAT 2
#define AP_CH_LOG 3

// 分片标志（constants.js 的 FLAG）
#define AP_FLAG_FIRST 0x01
#define AP_FLAG_LAST 0x02
#define AP_FLAG_ACK_REQ 0x04

// 单消息最多 16 片（seq 只有 4 位）；单消息上限 = 16 × 单片载荷。
#define AP_MAX_CHUNKS 16
// ★ 单条消息的**载荷**上限（字节）。这是两端必须共同遵守的硬约束。
//
// 为什么不能简单用 AP_MAX_CHUNKS × 单片载荷：那得到的是"理论最大"8192 字节，
// 而设备只有 400KB 内部 RAM、无 PSRAM，不可能为一条消息留 8KB 缓冲。
// 如果主机按 8192 发、设备只收 2048，表现是"任务列表永远显示不全"——
// 而且不会报错，只会静默截断，极难定位。
//
// 因此规则是：**主机在发送前必须保证载荷不超过此值**；设备按此值准备缓冲。
// 两端的这个数字由 packages/dsh-ai-passport/test/protocol-parity.test.js 卡死。
#define AP_MAX_PAYLOAD_BYTES 2048
// 接收缓冲与上限一致（+0：拼装缓冲本身就是上限大小）。
#define AP_RX_BUFFER_BYTES AP_MAX_PAYLOAD_BYTES
// 出站重传队列深度：只对要求 ACK 的控制消息排队。
#define AP_TX_RETRY_SLOTS 2

// ★ 重传缓冲按**实际报文的片数**定，而不是按 seq 位宽的理论上限。
//   控制报文的载荷上限是 AP_MAX_PAYLOAD_BYTES(2048)，协商到 244 字节/片时
//   需要 ceil(2048/244)=9 片；取 10 片留一点余量。
//   代价：协商出的 MTU 很小时（如未协商的 20 字节/片）大消息无法重传 ——
//   但那种情况本来也发不出去，会在 ap_link_send_json 里就被载荷上限拦下。
//   为什么必须收紧：按理论上限（16 片 × ~516B × 4 槽 = 33KB）会让 DRAM 不够用，
//   实测在加入截屏功能后链接器直接溢出。
#define AP_TX_RETRY_CHUNKS 10
#define AP_ACK_TIMEOUT_MS 400
#define AP_ACK_MAX_RETRIES 3

// ── 时序 ───────────────────────────────────────────────────────────────────
// 心跳周期与丢失上限（constants.js 的 HEARTBEAT_*）。
// 设备侧是**被动方**：收到 ping 回 pong，并用自己的表判断主机是否还活着。
#define AP_HEARTBEAT_INTERVAL_MS 5000
// ★ 从 3 放宽到 6：语音上行期间音频通知会把控制通道挤拥塞，ping/pong 可能丢。
//   3 次丢失（15 秒）就判掉线太紧，是"识别后有时自己断开连接"的原因之一。
//   6 次（30 秒）给语音突发留足余量；且 app_link.c 现在收到任何控制消息都会
//   重置计时，实际判掉线需要"整整 30 秒一条控制消息都没有"。
#define AP_HEARTBEAT_MISS_LIMIT 6

// ── 设备能力位（hello / CTRL 上报，与 constants.js 的 CAPABILITY 一致）────
#define AP_CAP_DISPLAY (1 << 0)
#define AP_CAP_BUTTONS (1 << 1)
#define AP_CAP_MIC (1 << 2)
#define AP_CAP_SPEAKER (1 << 3)
#define AP_CAP_BATTERY (1 << 4)
#define AP_CAP_NOTIFY_LED (1 << 5)
// v2：设备理解 session.list / session.detail / session.done。
// 主机据此选择 v2 会话路径或 v1 的 task.state 兼容路径。
#define AP_CAP_SESSION_LIST (1 << 6)

// ── 多会话（协议 v2）──
// 单次快照最多条数；标题 UTF-8 字节上限（主机侧按字符边界截断后下发）。
#define AP_SESSION_MAX 8
#define AP_SESSION_TITLE_BYTES 40
// handle 固定两位十六进制（"01".."08"），设备只认 handle。
#define AP_SESSION_HANDLE_LEN 2

// ── 消息类型 ───────────────────────────────────────────────────────────────
// 与 constants.js 的 MSG 表逐条对应。字符串比较在设备侧用 strcmp，
// 因此这些字面量必须与 JS 侧完全一致（包括点号位置）。
#define AP_MSG_HELLO "hello"
#define AP_MSG_HELLO_ACK "hello.ack"
#define AP_MSG_PING "ping"
#define AP_MSG_PONG "pong"
#define AP_MSG_ACK "ack"
#define AP_MSG_NACK "nack"


// ── 任务状态（精简版：设备只显示这四个之一）──────────────────────────────
//
// 精简前的设备端有完整任务列表、详情页、余额页。实际使用确认：240×320 上
// 列表"既看不全也读不快"，而用户真正需要的是"现在要不要我管一下"。
// 因此状态收敛成四态，由主机聚合后下发（设备不做任何判断）。
typedef enum {
    AP_TASK_STATE_IDLE = 0,              // 空闲：没有任务在跑
    AP_TASK_STATE_RUNNING,               // 运行中：正在执行，不用管
    AP_TASK_STATE_WAITING_APPROVAL,      // 待审批：需要按键，会响提示音
    AP_TASK_STATE_COMPLETED,             // 已完成：刚结束，会响提示音
} ap_task_state_t;

/** v1 兼容：状态上报 {state, title?}。v2 设备不再使用（改由 session.* 驱动）。 */
#define AP_MSG_TASK_STATE "task.state"

// ── 多会话（协议 v2 核心）──
// 载荷是**扁平标量**（设备端 JSON 取值器不解析嵌套数组/布尔）：
//   session.list  {n, handles:"01|02", titles:"标题|标题", states:"running|idle",
//                  unread:"0|1", sel:"02"}
//   session.detail {h, title, state, step, stepTotal, tool, elapsedMs, summary}
//   session.done   {h, title, result, durationMs}
// 字段内 `|` 转义为 `¦`，`\` 转义为 `╲`（见 app_proto.c 的 ap_flat_next）。
#define AP_MSG_SESSION_LIST "session.list"
#define AP_MSG_SESSION_LIST_REQ "session.list.req"
#define AP_MSG_SESSION_DETAIL "session.detail"
#define AP_MSG_SESSION_DETAIL_REQ "session.detail.req"
#define AP_MSG_SESSION_DONE "session.done"
#define AP_MSG_SESSION_SELECT "session.select"

#define AP_MSG_APPROVE_REQ "approve.req"
#define AP_MSG_APPROVE "approve"
#define AP_MSG_APPROVE_RESULT "approve.result"
// 追问/计划评审（docs/06 §3/§4）：主机逐题下发，设备增量回报选择。
#define AP_MSG_QUESTION_REQ "question.req"
#define AP_MSG_QUESTION_NAV "question.nav"
#define AP_MSG_QUESTION_PICK "question.pick"
#define AP_MSG_QUESTION_ANSWER "question.answer"
#define AP_MSG_QUESTION_DONE "question.done"

#define AP_MSG_BALANCE_REQ "balance.req"
#define AP_MSG_BALANCE "balance"

#define AP_MSG_VOICE_BEGIN "voice.begin"
#define AP_MSG_VOICE_END "voice.end"
#define AP_MSG_VOICE_RESULT "voice.result"
#define AP_MSG_VOICE_ERROR "voice.error"
// 识别结果卡的按键动作（设备 → Mac）：{resultId, mode:"fill"|"send"|"redo"}。
// 设备只报意图；真正动输入框的是主机侧挂件（docs/06 §2）。
#define AP_MSG_VOICE_ACTION "voice.action"

// ── 配对（认领 / 信任握手）────────────────────────────────────────────────
// 为什么放在应用层：主机侧走 noble，macOS 的 BLE bonding/passkey 体验与成功率都差，
// 且换主机 / 换设备要反复配对；应用层握手复用同一条 GATT，跨平台一致。
//
// 流程：设备未配对时屏幕显示一次性 6 位码（每次上电不同）→ 主机发 pair.begin{code,host}
//   → 设备校验（常量时间比较 + 每分钟最多 3 次）→ 通过则生成 token 存 NVS，回 pair.ok{deviceId,token}
//   → 之后主机每次 hello 带 token，设备校验通过才处理业务消息。
// 未配对期间设备**只接受** hello / ping / pair.*，其余业务消息回 pair.required（不执行）。
//
// 安全边界（如实记录）：码与 token 走**未加密**的 BLE 链路，这是"认领/信任"机制
// （防连错设备、防别人随手用你的设备），**不是**抗主动窃听的加密认证。
#define AP_MSG_PAIR_BEGIN "pair.begin"
#define AP_MSG_PAIR_OK "pair.ok"
#define AP_MSG_PAIR_FAILED "pair.failed"
#define AP_MSG_PAIR_REQUIRED "pair.required"
#define AP_MSG_PAIR_RESET "pair.reset"

// ── 调试消息（正式固件也保留：它只是把一次按键"喂"进同一条队列）────────
//
// 用途：没有按键注入手段时，"界面为什么不响应按键"只能靠人反复按 + 盯串口，
// 一轮十几分钟且无法自动化。有了它，主机可以直接驱动界面：
//     {"type":"debug.key","btn":0,"event":1}
// 走的是**与真实按键完全相同**的队列，因此测的是真实路径，不是旁路。
#define AP_MSG_DEBUG_KEY "debug.key"

#define AP_MSG_TOAST "toast"
#define AP_MSG_CONFIG "config"
// 桌面设置页「试听」：{kind:"approve"|...}，设备按音型播放一次。
#define AP_MSG_SOUND_TEST "sound.test"

// ── 任务状态 ───────────────────────────────────────────────────────────────
// 与 constants.js 的 TASK_STATUS 对应。设备屏幕上的状态胶囊直接映射这些值。
#define AP_STATUS_IDLE "idle"
#define AP_STATUS_RUNNING "running"
#define AP_STATUS_DONE "done"
#define AP_STATUS_ERROR "error"
#define AP_STATUS_ABORTED "aborted"
#define AP_STATUS_WAITING "waiting"

// ── 单会话状态（协议 v2）──
// 与 constants.js 的 SESSION_STATE 对应：设备按行显示图标/颜色，不显示状态文字。
#define AP_SSTATE_RUNNING "running"
#define AP_SSTATE_WAITING "waiting"
#define AP_SSTATE_ERROR "error"
#define AP_SSTATE_ABORTED "aborted"
#define AP_SSTATE_IDLE "idle"

// ── 试听音型（sound.test）──
#define AP_SOUND_APPROVE "approve"
#define AP_SOUND_COMPLETED "completed"
#define AP_SOUND_ERROR "error"
#define AP_SOUND_DECIDED "decided"
#define AP_SOUND_PAIR_OK "pair_ok"
#define AP_SOUND_PAIR_FAIL "pair_fail"
#define AP_SOUND_LOW_BATTERY "low_battery"

// ── 审批 ───────────────────────────────────────────────────────────────────
// 与 constants.js 的 DECISION 对应。
#define AP_DECISION_ALLOW "allow"
#define AP_DECISION_DENY "deny"

// 审批范围：
//   once   = 只允许这一次（界面上是"运行一次"）
//   always = 以后同类操作都不再询问（界面上是"总是运行"）
// ★ 用 scope 表达"总是运行"，而不是新增一个 decision 值 ——
//   决策（允许/拒绝）与范围（这次/以后）是两个正交的维度，
//   混成一个字段会让"以后都拒绝"这种组合无法表达。
#define AP_SCOPE_ONCE "once"
#define AP_SCOPE_ALWAYS "always"

// ── 语音参数 ───────────────────────────────────────────────────────────────
// 与 constants.js 的 AUDIO 对应。设备只负责采音 + 压缩 + 上行。
//
// ★ AP_AUDIO_MAX_SECONDS 是**单次录音时长的唯一真源**：app_voice.c 必须直接用它，
//   不要在 app_voice.c 里另写一份 MAX_MS。两份各写各的会静默漂移 ——
//   曾出现过 app_proto.h=15 而 app_voice.c=15、后来只改了后者的情况，
//   parity 测试照样通过（它比的是没人用的那份），真机行为与协议契约脱节。
#define AP_AUDIO_SAMPLE_RATE 16000
#define AP_AUDIO_FALLBACK_SAMPLE_RATE 8000
#define AP_AUDIO_BITS 16
#define AP_AUDIO_CHANNELS 1
#define AP_AUDIO_MAX_SECONDS 30
// 旧版有 AP_AUDIO_SILENCE_STOP_MS（静音自动结束门限），已随该功能删除：
// 录音只在「松手」或「触达 AP_AUDIO_MAX_SECONDS」时结束，句中停顿原样保留。

// 帧头各字段的位运算助手。写成内联函数而不是宏，便于加类型检查。
static inline uint8_t ap_head_byte0(uint8_t version, uint8_t flags)
{
    return (uint8_t)(((version & 0x0f) << 4) | (flags & 0x0f));
}

static inline uint8_t ap_head_byte3(uint16_t msg_id, uint8_t seq)
{
    return (uint8_t)(((msg_id & 0x0f) << 4) | (seq & 0x0f));
}

static inline uint8_t ap_head_version(const uint8_t *chunk)
{
    return (uint8_t)((chunk[0] >> 4) & 0x0f);
}

static inline uint8_t ap_head_flags(const uint8_t *chunk)
{
    return (uint8_t)(chunk[0] & 0x0f);
}

static inline uint16_t ap_head_msg_id(const uint8_t *chunk)
{
    return (uint16_t)(((uint16_t)chunk[2] << 4) | ((chunk[3] >> 4) & 0x0f));
}

static inline uint8_t ap_head_seq(const uint8_t *chunk)
{
    return (uint8_t)(chunk[3] & 0x0f);
}

// 发送前的容量校验：既要放得进分片数上限，也不能超过两端约定的载荷上限。
static inline bool ap_payload_fits(uint16_t chunk_payload, uint32_t bytes)
{
    if (bytes > AP_MAX_PAYLOAD_BYTES) return false;
    return bytes <= (uint32_t)chunk_payload * AP_MAX_CHUNKS;
}
