/**
 * dsh-ai-passport —— 客户端半区。
 *
 * 这个文件**必须保持为预先构建好的浏览器 bundle**，不能被 DSH 现场编译。
 * 格式由 `@deepseek-ai/dsh-client-modules` 规定：
 *
 *   1. 文件是一个 `window.__ModuleLoader__.load({ id, factory })` 调用；
 *   2. `id` 必须**严格等于包名**（对不上会在启动时被组合成一个响亮的错误）；
 *   3. factory 里用注入的 `require` 取共享依赖（只允许 React 与 jsx-runtime）；
 *   4. 导出 `apply(ctx)` 与 `inject`（硬依赖的服务名数组）。
 *
 * 因此这里刻意**不写 JSX**：没有构建步骤，就用 `React.createElement`。
 *
 * 职责（docs/09 §5）：
 *   · `settings.section` —— 设置侧栏里的独立一页 `AI Passport`；
 *   · `conversation.session.header.utilities` —— 会话标题旁的**三态指示器**。
 *
 * 旧的「输入框旁挂件」已删除：它占的是输入框工具行的 **single 槽**，
 * 与官方语音输入插件抢同一个座位，导致语音插件激活失败（docs/09 §8 P1-4）。
 * 挂件里的语音状态 / 配对视图判定是纯函数，仍有价值，继续导出给单测。
 *
 * 与宿主半区的通信：走**同源 HTTP**，而不是 Remote。
 * 原因：`ctx.remote.*` 那些命名空间由宿主侧 `@Remote` 装饰器在 DSH 自己的构建期生成，
 * 外部包无法注册自己的命名空间。宿主半区已经用 `ctx.webServer.register()` 挂了
 * `/dsh-passport/*` 路由，客户端与页面同源，直接 fetch 即可。
 */

window.__ModuleLoader__.load({
  id: 'dsh-ai-passport',
  factory: (require) => {
    const React = require('react')
    const { createElement: h, useState, useEffect, useRef, useCallback } = React

    /** 面板接口前缀，与宿主半区 `lib/panel/index.js` 注册的路径一致。 */
    const API = '/dsh-passport'
    /** Web 控制面板入口（设置页缺失时的替代入口，也是排障用的独立页面）。 */
    const PANEL_PATH = '/dsh-passport'
    /** 插件版本。与 package.json 的 version 对齐（单测会钉住，避免两处漂移）。 */
    const VERSION = '2.0.0'
    /** 轮询间隔：可见 2s；标签页隐藏时降频到 10s（指示器/设置页都不该在后台空转）。 */
    const POLL_INTERVAL_MS = 2000
    const POLL_HIDDEN_MS = 10000
    /** `session.focus` 上报节流：同一窗口 1s 内最多上报一次（docs/09 §5.2）。 */
    const FOCUS_THROTTLE_MS = 1000
    /** 设备列表最多会话数的上限（设备屏只有 240×320，协议给到 8）。 */
    const MAX_SESSIONS = 8

    /** 设置页与设备共用的可写项默认值；宿主侧 `lib/settings-store.js` 是唯一事实源。 */
    const SETTINGS_DEFAULTS = Object.freeze({
      maskTitles: false,
      soundEnabled: true,
      backlightPulse: true,
      detailSummary: true,
      detailElapsed: true,
      voiceAutoSend: false,
      maxSessions: MAX_SESSIONS,
    })

    /** 链路状态 → 中文。 */
    const STATE_TEXT = {
      idle: '未连接',
      scanning: '扫描中',
      connecting: '连接中',
      connected: '已连接',
      reconnecting: '重连中',
      failed: '异常',
    }

    /** 试听音型：`sound.test` 的 kind → 按钮文案（与宿主 SOUND_KIND 对齐）。 */
    const SOUND_TESTS = [
      ['approve', '待审批'],
      ['completed', '会话完成'],
      ['error', '出错'],
      ['decided', '已选择'],
    ]

    /** 样式；用 CSS 变量取主题色，深浅色模式都能跟随。 */
    const CSS = `
.ap-page{display:flex;flex-direction:column;gap:12px;max-width:620px;font-size:12px;line-height:1.5;color:var(--dsw-alias-text-l1, inherit)}
.ap-notice{margin-bottom:2px;padding:6px 9px;border:1px solid var(--dsw-alias-border-l2, rgba(128,128,128,.28));border-radius:8px;font-size:11px;line-height:1.4;word-break:break-word}
.ap-notice[data-error="1"]{border-color:rgba(220,38,38,.5);color:#dc2626}
.ap-card{border:1px solid var(--dsw-alias-border-l2, rgba(128,128,128,.24));border-radius:10px;padding:12px 14px;background:var(--dsw-alias-bg-layer-1, transparent)}
.ap-card-h{display:flex;align-items:center;gap:8px;margin:0 0 8px;font-size:12px;font-weight:600}
.ap-card-h .ap-state{margin-left:2px;font-size:11px;font-weight:400;color:var(--dsw-alias-text-l3, #6b7280)}
.ap-card-h .ap-state[data-on="1"]{color:#16a34a}
.ap-sub-h{margin:12px 0 4px;font-size:11px;font-weight:600;color:var(--dsw-alias-text-l3, #6b7280)}
.ap-kv{display:grid;grid-template-columns:repeat(2, minmax(0, 1fr));gap:8px 16px;margin:0}
.ap-kv-item{min-width:0}
.ap-kv-item .k{display:block;font-size:10px;line-height:1.3;color:var(--dsw-alias-text-l3, #6b7280)}
.ap-kv-item .v{display:block;font-size:12px;font-weight:600;line-height:1.5;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;font-variant-numeric:tabular-nums}
.ap-row{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:7px 0;border-bottom:1px solid var(--dsw-alias-border-l3, rgba(128,128,128,.14))}
.ap-row:last-child{border-bottom:none}
.ap-row .ap-label{flex:1;min-width:0}
.ap-row .ap-desc{display:block;margin-top:1px;font-size:11px;line-height:1.35;color:var(--dsw-alias-text-l3, #6b7280)}
.ap-row .ap-val{flex:none;max-width:60%;text-align:right;font-size:12px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.ap-switch{position:relative;flex:none;width:34px;height:20px;padding:0;border-radius:10px;border:1px solid var(--dsw-alias-border-l2, rgba(128,128,128,.32));background:var(--dsw-alias-bg-layer-2, rgba(128,128,128,.16));cursor:pointer;transition:background .15s, border-color .15s}
.ap-switch .ap-knob{position:absolute;top:2px;left:2px;width:14px;height:14px;border-radius:50%;background:#fff;box-shadow:0 1px 2px rgba(0,0,0,.25);transition:left .15s}
.ap-switch[data-on="1"]{background:#16a34a;border-color:#16a34a}
.ap-switch[data-on="1"] .ap-knob{left:16px}
.ap-switch:disabled{opacity:.45;cursor:default}
.ap-select{flex:none;min-width:64px;padding:3px 6px;border:1px solid var(--dsw-alias-border-l2, rgba(128,128,128,.28));border-radius:6px;background:var(--dsw-alias-bg-layer-1, transparent);color:inherit;font-size:12px;font-family:inherit;cursor:pointer}
.ap-actions{display:flex;flex-wrap:wrap;gap:6px;margin-top:8px}
.ap-btn{border:1px solid var(--dsw-alias-border-l2, rgba(128,128,128,.28));background:transparent;color:inherit;border-radius:6px;padding:3px 9px;font-size:11px;line-height:1.6;cursor:pointer;font-family:inherit}
.ap-btn:hover:not(:disabled){background:var(--dsw-alias-bg-layer-2, rgba(128,128,128,.1))}
.ap-btn.primary{background:#4d6bfe;border-color:#4d6bfe;color:#fff}
.ap-btn.primary:hover:not(:disabled){filter:brightness(1.08)}
.ap-btn:disabled{opacity:.45;cursor:default}
.ap-input{flex:1;min-width:0;box-sizing:border-box;border:1px solid var(--dsw-alias-border-l2, rgba(128,128,128,.28));background:transparent;color:inherit;border-radius:6px;padding:3px 6px;font-size:12px;font-family:ui-monospace, SFMono-Regular, Menlo, monospace;letter-spacing:2px}
.ap-input:focus{outline:1px solid #4d6bfe;border-color:#4d6bfe}
.ap-err{color:#dc2626;font-size:11px;line-height:1.4}
.ap-muted{color:var(--dsw-alias-text-l3, #6b7280)}
.ap-small{font-size:11px;line-height:1.5}
.ap-legend{display:flex;flex-wrap:wrap;gap:4px 12px;margin-top:8px;font-size:11px;color:var(--dsw-alias-text-l3, #6b7280)}
.ap-legend i{display:inline-flex;align-items:center;gap:4px;font-style:normal}
.ap-link{color:#4d6bfe;text-decoration:none}
.ap-link:hover{text-decoration:underline}
.ap-dot{width:7px;height:7px;border-radius:50%;background:#9aa1ab;flex:none}
.ap-dot[data-tone="green"]{background:#16a34a}
.ap-dot[data-tone="yellow"]{background:#d97706}
.ap-dot[data-tone="red"]{background:#dc2626}
.ap-ind{display:inline-flex;align-items:center;gap:6px;max-width:140px;height:24px;padding:0 8px;border:1px solid var(--dsw-alias-border-l2, rgba(128,128,128,.28));border-radius:999px;background:transparent;color:var(--dsw-alias-text-l2, inherit);font-size:11px;line-height:1;white-space:nowrap;cursor:default;overflow:hidden}
.ap-ind-text{overflow:hidden;text-overflow:ellipsis}
.ap-ind[data-tone="connected"]{border-color:color-mix(in srgb, #4d6bfe 42%, transparent)}
.ap-ind[data-tone="connected"] .ap-dot{background:#4d6bfe}
.ap-ind[data-tone="active"]{border-color:color-mix(in srgb, #16a34a 45%, transparent)}
.ap-ind[data-tone="active"] .ap-dot{background:#16a34a}
`

    // ─────────────────────────── 纯函数视图（单测直接钉住） ───────────────────────────

    /**
     * 语音区显示决策（纯函数）。
     *
     * 规则与主机 voice.prepare() 的前置条件严格对齐 —— 不给做不到的按钮：
     *   · 已就绪            → 「已启用」，不显示按钮
     *   · 有识别提供者未就绪 → 「未启用」，显示「准备模型」
     *   · 无提供者 / 无服务  → 「未启用」，不显示按钮（此时 prepare() 必然抛错）
     */
    function speechViewFor(state) {
      const speech = state?.speech ?? null
      const ready = speech?.ready === true
      // 「准备模型」只在"确实没装 / 上次准备失败"时出现（主机给 needsPreparation）。
      // 老宿主没有该字段时退化为旧规则，避免新旧半区搭配时按钮整个消失。
      const needsPreparation = speech?.needsPreparation === true
        || (speech?.needsPreparation === undefined && Boolean(speech?.providerId) && !ready)
      return {
        ready,
        text: ready ? '已启用' : '未启用',
        // 说明行只用于必须解释的状态：出错、或正在准备（下载/加载进度）
        hint: state?.speechError
          ? String(state.speechError)
          : (speech?.preparing === true && speech?.detail ? String(speech.detail) : null),
        showPrepare: needsPreparation && !ready,
      }
    }

    /**
     * 配对区的视图模型（纯函数）：
     *   needed=true  → 显示 6 位码输入框 + 「配对」按钮
     *   已配对       → 显示设备 ID + 「解除配对」
     *   error        → 失败原因（码错/限流/发送失败）
     */
    function pairingViewFor(state) {
      const pairing = state?.pairing
      if (!pairing) return { show: false }
      const errText = {
        'bad-code': '配对码不对，请照设备屏幕重输',
        'rate-limited': '尝试太频繁，请等一分钟再试',
        'send-failed': '发送失败，请检查蓝牙连接',
        'bad-input': '请输入 6 位数字',
        'token-mismatch': '本机未被该设备信任，需要重新配对',
        'empty-token': '设备返回的 token 为空，配对失败',
      }[pairing.error] ?? null
      return {
        show: true,
        needed: Boolean(pairing.needed),
        paired: Boolean(pairing.paired),
        deviceId: pairing.deviceId ?? null,
        hostName: pairing.hostName ?? null,
        remaining: pairing.remaining ?? null,
        error: errText,
      }
    }

    /** 把宿主给的 settings 归一化：缺字段补默认值、布尔项只认布尔、maxSessions 钳到 1–8。 */
    function normalizeSettings(raw) {
      const result = { ...SETTINGS_DEFAULTS }
      if (!raw || typeof raw !== 'object') return result
      for (const key of Object.keys(SETTINGS_DEFAULTS)) {
        if (raw[key] === undefined) continue
        if (key === 'maxSessions') {
          const value = Number(raw[key])
          if (Number.isFinite(value)) {
            result.maxSessions = Math.min(MAX_SESSIONS, Math.max(1, Math.trunc(value)))
          }
        } else if (typeof raw[key] === 'boolean') {
          result[key] = raw[key]
        }
      }
      return result
    }

    /** 按 sessionId 查会话标题（快照里的 sessions 只含设备可见的那几条）。 */
    function sessionTitleFor(state, sessionId) {
      if (!sessionId) return ''
      const sessions = Array.isArray(state?.sessions) ? state.sessions : []
      const found = sessions.find((session) => session?.sessionId === sessionId)
      return found ? String(found.title ?? '') : ''
    }

    /**
     * 设置页视图模型（纯函数）。
     *
     * 所有渲染所需数据在这里收口：组件只负责把字段贴到 DOM 上，
     * 单测也能在"没有 React 运行时"的情况下断言完整状态。
     */
    function settingsViewFor(state) {
      const link = state?.link ?? {}
      const linkState = link.state ?? 'idle'
      const settings = normalizeSettings(state?.settings)
      const sessions = Array.isArray(state?.sessions) ? state.sessions : []
      const deviceSelectedSessionId = String(state?.deviceSelectedSessionId ?? '')
      const focusedSessionId = String(state?.desktopFocusedSessionId ?? '')
      const protocolVersion = link.deviceInfo?.protocolVersion
      return {
        version: VERSION,
        linkState,
        linkStateText: STATE_TEXT[linkState] ?? linkState,
        connected: linkState === 'connected',
        deviceName: link.device?.name ?? 'AI Passport',
        firmware: link.deviceInfo?.firmware ?? '',
        batteryPercent: link.deviceInfo?.batteryPercent ?? null,
        rssi: link.device?.rssi ?? null,
        protocolVersion: protocolVersion != null ? Number(protocolVersion) : null,
        handshakeComplete: Boolean(link.handshakeComplete),
        lastError: link.lastError ?? null,
        diagnose: state?.diagnose ?? null,
        transport: state?.transport ?? '',
        // 宿主快照目前没带 autoConnect；拿不到就显示"—"，不要瞎猜成"关"。
        autoConnect: typeof state?.config?.autoConnect === 'boolean' ? state.config.autoConnect : null,
        autoReconnect: Boolean(state?.config?.autoReconnect),
        targetDeviceId: String(state?.config?.targetDeviceId ?? ''),
        speechProviderId: String(state?.config?.voice?.providerId ?? ''),
        settings,
        speech: speechViewFor(state),
        pairing: pairingViewFor(state),
        sessionCount: sessions.length,
        deviceSelectedSessionId,
        deviceSelectedSessionTitle: sessionTitleFor(state, deviceSelectedSessionId),
        focusedSessionId,
        focusedSessionTitle: sessionTitleFor(state, focusedSessionId),
      }
    }

    /**
     * 会话旁指示器的三态视图模型（纯函数，docs/09 §5.2）：
     *   设备未连接               → tone=offline，灰点「AI Passport 未连接」
     *   已连接但设备选中的不是本会话 → tone=connected，蓝点「设备已连接」
     *   已连接且设备选中的是本会话   → tone=active，绿点「本会话已在设备上」
     */
    function indicatorViewFor(state, sessionId) {
      const link = state?.link ?? {}
      const connected = (link.state ?? 'idle') === 'connected'
      const deviceName = link.device?.name ?? 'AI Passport'
      const battery = link.deviceInfo?.batteryPercent
      const protocol = link.deviceInfo?.protocolVersion
      const title = connected
        ? [deviceName, battery != null ? `电量 ${battery}%` : null, protocol != null ? `协议 v${protocol}` : null]
          .filter(Boolean).join(' · ')
        : `${deviceName} · 未连接`
      if (!connected) return { tone: 'offline', text: 'AI Passport 未连接', title }
      const selected = String(state?.deviceSelectedSessionId ?? '')
      const current = String(sessionId ?? '')
      if (current && selected && current === selected) return { tone: 'active', text: '本会话已在设备上', title }
      return { tone: 'connected', text: '设备已连接', title }
    }

    /** 轮询间隔：页面隐藏时降频（纯函数，便于单测）。 */
    function pollIntervalFor(hidden) {
      return hidden ? POLL_HIDDEN_MS : POLL_INTERVAL_MS
    }

    /** 距下次允许上报还剩多少毫秒；0 表示立刻发（纯函数，便于单测）。 */
    function focusDelayFor(lastSentAt, now) {
      const elapsed = now - (Number(lastSentAt) || 0)
      return elapsed >= FOCUS_THROTTLE_MS ? 0 : FOCUS_THROTTLE_MS - elapsed
    }

    // ─────────────────────────────── 同源 HTTP ───────────────────────────────

    /** 面板接口调用。失败一律吞掉并返回 null——客户端半区不该因为后端没起来就报错刷屏。 */
    async function callApi(path, init) {
      try {
        const response = await fetch(`${API}${path}`, init)
        if (!response.ok) return null
        return await response.json()
      } catch {
        return null
      }
    }

    /** 发一个宿主动作，返回 `{ ok, result }` 或 null。 */
    function act(action, payload) {
      return callApi('/action', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ action, payload: payload ?? {} }),
      })
    }

    // ───────────────────────────────── hooks ─────────────────────────────────

    /**
     * 轮询状态。
     *
     * 为什么不用 SSE：指示器常驻、设置页也常驻，SSE 连接会一直占着；
     * 需要的信息量很小（几百字节），2 秒轮询在桌面上完全无感。
     */
    function useApiState() {
      const [state, setState] = useState(null)
      useEffect(() => {
        let alive = true
        let timer = null
        const tick = async () => {
          const body = await callApi('/state.json')
          if (!alive) return
          if (body?.ok) setState(body.state)
          const hidden = typeof document !== 'undefined' && document.hidden === true
          timer = setTimeout(tick, pollIntervalFor(hidden))
        }
        void tick()
        return () => {
          alive = false
          if (timer) clearTimeout(timer)
        }
      }, [])
      return state
    }

    /**
     * 聚焦会话上报（`session.focus`）。
     *
     * 挂载/`sessionId` 变化时上报一次，1s 节流：多窗口切换会话时 last-wins，
     * 但不会因为快速切换把宿主刷爆。定时器随 effect 清理，不泄漏。
     */
    function useSessionFocus(sessionId) {
      const lastSentRef = useRef(0)
      const pendingRef = useRef('')
      const timerRef = useRef(null)
      useEffect(() => {
        if (!sessionId) return undefined
        pendingRef.current = String(sessionId)
        const flush = () => {
          timerRef.current = null
          lastSentRef.current = Date.now()
          void act('session.focus', { sessionId: pendingRef.current })
        }
        const wait = focusDelayFor(lastSentRef.current, Date.now())
        if (wait === 0) flush()
        else if (!timerRef.current) timerRef.current = setTimeout(flush, wait)
        return () => {
          if (timerRef.current) {
            clearTimeout(timerRef.current)
            timerRef.current = null
          }
        }
      }, [sessionId])
    }

    // ─────────────────────────────── UI 原子件 ───────────────────────────────

    /** 卡片：一页设置里的一个分区。 */
    function Card(props) {
      const { title, badge, badgeOn, children } = props
      return h(
        'section',
        { className: 'ap-card' },
        h(
          'div',
          { className: 'ap-card-h' },
          h('span', null, title),
          badge != null
            ? h('span', { className: 'ap-state', 'data-on': badgeOn ? '1' : '0' }, badge)
            : null,
        ),
        children,
      )
    }

    /** 一行设置：左侧标签+说明，右侧控件。 */
    function Row(props) {
      const { label, desc, children } = props
      return h(
        'div',
        { className: 'ap-row' },
        h(
          'div',
          { className: 'ap-label' },
          h('span', null, label),
          desc ? h('span', { className: 'ap-desc' }, desc) : null,
        ),
        children ?? null,
      )
    }

    /** 键值格（设备信息用）。 */
    function KvCell(props) {
      return h(
        'div',
        { className: 'ap-kv-item' },
        h('span', { className: 'k' }, props.k),
        h('span', { className: 'v' }, props.v),
      )
    }

    /** 开关（role=switch 的按钮，键盘/读屏可用）。 */
    function Toggle(props) {
      const { on, disabled, label, onChange } = props
      return h(
        'button',
        {
          type: 'button',
          className: 'ap-switch',
          role: 'switch',
          'aria-checked': on ? 'true' : 'false',
          'aria-label': label,
          'data-on': on ? '1' : '0',
          disabled: Boolean(disabled),
          onClick: onChange,
        },
        h('span', { className: 'ap-knob' }),
      )
    }

    // ─────────────────────────────── 设置页 ───────────────────────────────

    /**
     * 设置侧栏里的 AI Passport 独立页（`settings.section`，docs/09 §5.1）。
     *
     * `props.stateOverride` 只给离线渲染/单测注入快照用；DSH 宿主只传 `close`。
     */
    function SettingsPage(props) {
      const polled = useApiState()
      const state = props?.stateOverride ?? polled
      const [busy, setBusy] = useState(false)
      const [notice, setNotice] = useState(null)
      const [overrides, setOverrides] = useState({})
      const [pairCode, setPairCode] = useState('')

      const view = settingsViewFor(state)
      const settings = { ...view.settings, ...overrides }

      // 宿主的设置变了（例如设备端改了开关）→ 已经落地的本地乐观值让位给宿主值。
      // 注意：只在"确实丢掉某个键"时返回新对象，否则返回同一个引用，
      // 避免 effect 每轮都触发一次重渲染（remoteSettings 每次渲染都是新对象）。
      const remoteSettings = view.settings
      useEffect(() => {
        setOverrides((pending) => {
          const keys = Object.keys(pending)
          if (keys.length === 0) return pending
          const next = {}
          let dropped = false
          for (const key of keys) {
            if (remoteSettings[key] !== pending[key]) next[key] = pending[key]
            else dropped = true
          }
          return dropped ? next : pending
        })
      }, [remoteSettings])

      // 配对这类"提交后等设备回话"的动作，结果消息会被下一轮询取代：
      // 配对状态一变就清掉过渡提示（不再挂着"已提交配对码，等待设备确认…"）。
      const pairingKey = `${state?.pairing?.paired ? 1 : 0}:${state?.pairing?.needed ? 1 : 0}`
      useEffect(() => {
        setNotice(null)
      }, [pairingKey])

      /** 执行一个宿主动作，并把结果显式显示出来（静默失败是最难查的那类 bug）。 */
      const run = useCallback(async (action, payload, fallbackText) => {
        setBusy(true)
        setNotice(null)
        try {
          const body = await act(action, payload)
          if (!body) throw new Error('宿主未响应')
          if (body.ok === false) throw new Error(body.message ?? '操作失败')
          setNotice({ text: body?.result?.message ?? fallbackText ?? '已完成', error: false })
          return body?.result ?? null
        } catch (error) {
          setNotice({ text: `请求失败：${error?.message ?? error}`, error: true })
          return null
        } finally {
          setBusy(false)
        }
      }, [])

      /** 保存设置：先乐观更新，失败回滚；宿主归一化后的值以宿主为准。 */
      const save = useCallback(async (patch) => {
        setOverrides((prev) => ({ ...prev, ...patch }))
        const result = await run('settings.set', { patch }, '已保存并生效')
        if (result) {
          const normalized = normalizeSettings(result.settings)
          const applied = {}
          for (const key of Object.keys(patch)) applied[key] = normalized[key]
          setOverrides((prev) => ({ ...prev, ...applied }))
        } else {
          setOverrides((prev) => {
            const next = { ...prev }
            for (const key of Object.keys(patch)) delete next[key]
            return next
          })
        }
      }, [run])

      const selectedText = view.deviceSelectedSessionId
        ? (view.deviceSelectedSessionTitle || '已选中（标题同步中）')
        : '未选择，在设备上用上下键选择'
      const focusedText = view.focusedSessionId
        ? (view.focusedSessionTitle || '已上报（标题同步中）')
        : '尚未上报'
      const sessionCount = `设备可见 ${view.sessionCount} 条`

      return h(
        'div',
        { className: 'ap-page' },

        notice
          ? h(
              'div',
              { className: 'ap-notice', 'data-error': notice.error ? '1' : '0', role: 'status' },
              notice.text,
            )
          : null,

        // ── 1. 连接 ──
        h(
          Card,
          { title: '连接', badge: view.linkStateText, badgeOn: view.connected },
          h(
            'div',
            { className: 'ap-kv' },
            h(KvCell, { k: '设备', v: view.deviceName }),
            h(KvCell, { k: '固件', v: view.firmware || '—' }),
            h(KvCell, { k: '电量', v: view.batteryPercent != null ? `${view.batteryPercent}%` : '—' }),
            h(KvCell, { k: '信号', v: view.rssi != null ? `${view.rssi} dBm` : '—' }),
            h(KvCell, { k: '握手', v: view.connected ? (view.handshakeComplete ? '完成' : '进行中') : '—' }),
            h(KvCell, { k: '协议版本', v: view.protocolVersion != null ? `v${view.protocolVersion}` : '—' }),
          ),
          !view.connected && view.lastError
            ? h('div', { className: 'ap-err' }, view.lastError.message)
            : null,
          !view.connected && view.lastError?.hint
            ? h('div', { className: 'ap-err' }, view.lastError.hint)
            : null,
          view.diagnose
            ? h(
                'div',
                { className: 'ap-muted ap-small', style: { marginTop: 8 } },
                `适配器 ${view.diagnose.adapterState ?? '未上报'} · 桥进程 ${view.diagnose.childRunning ? '运行中' : '未运行'} · noble ${view.diagnose.noble ?? '未加载'}`,
              )
            : null,
          h(
            'div',
            { className: 'ap-actions' },
            !view.connected
              ? h(
                  'button',
                  {
                    className: 'ap-btn primary',
                    type: 'button',
                    disabled: busy,
                    onClick: () => run('device.connect', undefined, '已发起扫描，等待设备广播…'),
                  },
                  busy ? '连接中…' : '扫描并连接',
                )
              : h(
                  'button',
                  {
                    className: 'ap-btn',
                    type: 'button',
                    disabled: busy,
                    onClick: () => run('device.disconnect', undefined, '已断开连接'),
                  },
                  '断开',
                ),
            h(
              'button',
              { className: 'ap-btn', type: 'button', disabled: busy, onClick: () => run('device.diagnose') },
              '诊断蓝牙',
            ),
            h(
              'a',
              { className: 'ap-btn', href: PANEL_PATH, target: '_blank', rel: 'noreferrer' },
              '控制面板',
            ),
          ),
          h('div', { className: 'ap-sub-h' }, '启动项（只读，改动需重启 DSH）'),
          h(
            'div',
            { className: 'ap-kv' },
            h(KvCell, { k: '传输方式', v: view.transport || '—' }),
            h(KvCell, { k: '自动连接', v: view.autoConnect == null ? '—' : view.autoConnect ? '开' : '关' }),
            h(KvCell, { k: '自动重连', v: view.autoReconnect ? '开' : '关' }),
            h(KvCell, { k: '目标设备', v: view.targetDeviceId || '未指定' }),
            h(KvCell, { k: '识别提供者', v: view.speechProviderId || '自动' }),
          ),
        ),

        // ── 2. 配对 ──
        view.pairing.show
          ? h(
              Card,
              { title: '配对', badge: view.pairing.paired ? '已配对' : '未配对', badgeOn: view.pairing.paired },
              view.pairing.paired
                ? h(
                    'div',
                    null,
                    h(
                      'div',
                      { className: 'ap-muted ap-small', style: { marginBottom: 6 } },
                      view.pairing.deviceId ? `设备 ${view.pairing.deviceId}` : '设备已信任本机',
                    ),
                    h(
                      'button',
                      {
                        className: 'ap-btn',
                        type: 'button',
                        disabled: busy,
                        onClick: () => run('pair.unpair', undefined, '已解除配对'),
                      },
                      '解除配对',
                    ),
                  )
                : h(
                    'div',
                    null,
                    h(
                      'div',
                      { className: 'ap-muted ap-small', style: { marginBottom: 6 } },
                      '输入设备屏幕上的 6 位配对码：',
                    ),
                    h(
                      'div',
                      { style: { display: 'flex', gap: 6 } },
                      h('input', {
                        className: 'ap-input',
                        type: 'text',
                        inputMode: 'numeric',
                        maxLength: 6,
                        placeholder: '6 位数字',
                        value: pairCode,
                        onChange: (event) => setPairCode(String(event.target.value ?? '').replace(/\D/g, '').slice(0, 6)),
                        onKeyDown: (event) => {
                          if (event.key === 'Enter' && pairCode.length === 6) {
                            void run('pair.submit', { code: pairCode }, '已提交配对码，等待设备确认…')
                          }
                        },
                      }),
                      h(
                        'button',
                        {
                          className: 'ap-btn primary',
                          type: 'button',
                          disabled: busy || pairCode.length !== 6,
                          onClick: () => run('pair.submit', { code: pairCode }, '已提交配对码，等待设备确认…'),
                        },
                        '配对',
                      ),
                    ),
                    view.pairing.remaining != null && view.pairing.remaining <= 1
                      ? h('div', { className: 'ap-err ap-small', style: { marginTop: 6 } }, `剩余尝试次数：${view.pairing.remaining}`)
                      : null,
                  ),
              view.pairing.error ? h('div', { className: 'ap-err', style: { marginTop: 6 } }, view.pairing.error) : null,
            )
          : null,

        // ── 3. 设备显示 ──
        h(
          Card,
          { title: '设备显示' },
          h(
            Row,
            { label: '标题打码', desc: '设备列表里会话名显示为「会话 1/2/3」' },
            h(Toggle, {
              on: settings.maskTitles,
              disabled: busy,
              label: '标题打码',
              onChange: () => save({ maskTitles: !settings.maskTitles }),
            }),
          ),
          h(
            Row,
            { label: '提示音', desc: '审批、完成、出错时设备响提示音' },
            h(Toggle, {
              on: settings.soundEnabled,
              disabled: busy,
              label: '提示音',
              onChange: () => save({ soundEnabled: !settings.soundEnabled }),
            }),
          ),
          h(
            Row,
            { label: '背光脉冲', desc: '提醒到达时屏幕闪一下' },
            h(Toggle, {
              on: settings.backlightPulse,
              disabled: busy,
              label: '背光脉冲',
              onChange: () => save({ backlightPulse: !settings.backlightPulse }),
            }),
          ),
          h(
            Row,
            { label: '最多会话', desc: '设备列表最多显示 1–8 条' },
            h(
              'select',
              {
                className: 'ap-select',
                value: String(settings.maxSessions),
                disabled: busy,
                onChange: (event) => save({ maxSessions: Number(event.target.value) }),
              },
              Array.from({ length: MAX_SESSIONS }, (_, index) => index + 1).map((count) =>
                h('option', { key: String(count), value: String(count) }, String(count)),
              ),
            ),
          ),
          h(
            Row,
            { label: '详情·最近输出', desc: '会话页显示最近一条助手输出摘要' },
            h(Toggle, {
              on: settings.detailSummary,
              disabled: busy,
              label: '详情最近输出',
              onChange: () => save({ detailSummary: !settings.detailSummary }),
            }),
          ),
          h(
            Row,
            { label: '详情·耗时', desc: '会话页显示本轮耗时' },
            h(Toggle, {
              on: settings.detailElapsed,
              disabled: busy,
              label: '详情耗时',
              onChange: () => save({ detailElapsed: !settings.detailElapsed }),
            }),
          ),
          h('div', { className: 'ap-sub-h' }, '提示音试听'),
          h(
            'div',
            { className: 'ap-actions', style: { marginTop: 0 } },
            SOUND_TESTS.map(([kind, label]) =>
              h(
                'button',
                {
                  key: kind,
                  className: 'ap-btn',
                  type: 'button',
                  disabled: busy || !view.connected,
                  title: view.connected ? `播放「${label}」提示音` : '设备未连接',
                  onClick: () => run('sound.test', { kind }, '已发送试听'),
                },
                label,
              ),
            ),
          ),
          h(
            'div',
            { className: 'ap-legend' },
            h('i', null, h('span', { className: 'ap-dot', 'data-tone': 'green' }), '运行中'),
            h('i', null, h('span', { className: 'ap-dot', 'data-tone': 'yellow' }), '等待你处理'),
            h('i', null, h('span', { className: 'ap-dot', 'data-tone': 'red' }), '出错/中断'),
            h('i', null, h('span', { className: 'ap-dot' }), '空闲'),
          ),
        ),

        // ── 4. 语音 ──
        h(
          Card,
          { title: '语音', badge: view.speech.text, badgeOn: view.speech.ready },
          view.speech.hint
            ? h('div', { className: view.speech.ready ? 'ap-muted ap-small' : 'ap-err' }, view.speech.hint)
            : null,
          view.speech.showPrepare
            ? h(
                'div',
                { className: 'ap-actions', style: { marginTop: 8 } },
                h(
                  'button',
                  {
                    className: 'ap-btn',
                    type: 'button',
                    disabled: busy,
                    onClick: () => run('speech.prepare', undefined, '已开始准备识别模型'),
                  },
                  '准备模型',
                ),
              )
            : null,
          h(
            Row,
            { label: '自动发送', desc: '识别完不再确认，直接发到设备选中的会话' },
            h(Toggle, {
              on: settings.voiceAutoSend,
              disabled: busy,
              label: '自动发送',
              onChange: () => save({ voiceAutoSend: !settings.voiceAutoSend }),
            }),
          ),
        ),

        // ── 5. 会话接入 ──
        h(
          Card,
          { title: '会话接入' },
          h(Row, { label: '设备选中会话', desc: '在设备上用上下键选择' }, h('span', { className: 'ap-val' }, selectedText)),
          h(Row, { label: '桌面聚焦会话', desc: '最近一次由会话窗口上报' }, h('span', { className: 'ap-val' }, focusedText)),
          h(Row, { label: '会话数量' }, h('span', { className: 'ap-val' }, sessionCount)),
        ),

        // ── 6. 关于 ──
        h(
          Card,
          { title: '关于' },
          h(Row, { label: '插件版本' }, h('span', { className: 'ap-val' }, view.version)),
          h(
            Row,
            { label: '设备协议版本' },
            h('span', { className: 'ap-val' }, view.protocolVersion != null ? `v${view.protocolVersion}` : '未连接'),
          ),
          h(Row, { label: '许可' }, h('span', { className: 'ap-val' }, 'MIT')),
          h(
            Row,
            { label: '控制面板' },
            h('a', { className: 'ap-link ap-small', href: PANEL_PATH, target: '_blank', rel: 'noreferrer' }, PANEL_PATH),
          ),
          h(
            'div',
            { className: 'ap-muted ap-small', style: { marginTop: 8 } },
            '设置保存后立即生效；蓝牙连接、传输方式等启动项需要重启 DSH。',
          ),
        ),
      )
    }

    // ─────────────────────────── 会话旁三态指示器 ───────────────────────────

    /**
     * 会话标题右侧的状态 chip（`conversation.session.header.utilities`，list 槽）。
     *
     * 从 `props.sessionId` 知道自己在哪个会话旁边；挂载/变化时上报聚焦会话。
     * `props.stateOverride` 同样只给离线渲染/单测用。
     */
    function SessionIndicator(props) {
      const polled = useApiState()
      const state = props?.stateOverride ?? polled
      const sessionId = props?.sessionId
      useSessionFocus(sessionId)
      const view = indicatorViewFor(state, sessionId)
      return h(
        'span',
        { className: 'ap-ind', 'data-tone': view.tone, title: view.title, role: 'status' },
        h('span', { className: 'ap-dot' }),
        h('span', { className: 'ap-ind-text' }, view.text),
      )
    }

    // ─────────────────────────────── 注册/导出 ───────────────────────────────

    /** 硬依赖：只有槽位注册表。不声明 locale（没装字典时声明它会直接报错）。 */
    const inject = ['slots']

    const SETTINGS_SLOT = 'settings.section'
    const INDICATOR_SLOT = 'conversation.session.header.utilities'

    /** 客户端半区的日志：没有 logger 服务可用，控制台是唯一出口。 */
    function warn(message) {
      try {
        console.warn(message)
      } catch {
        // 控制台不可用就算了：客户端半区不该因为日志失败影响 DSH 启动
      }
    }

    /**
     * 注册一个槽位条目。
     *
     * `slots.inject()` 会在槽位被声明后回调（DSH 各 UI 包的激活顺序不受我们控制）；
     * 如果当前 DSH 版本没有这个槽位，回调可能永远不执行 —— 此时功能降级，
     * 但**不能**把异常抛到 apply() 里，否则整个客户端包组合失败（docs/09 §5.4）。
     */
    function installSlot(ctx, options, Component) {
      const slotName = options.name
      try {
        ctx.slots.inject(slotName, () => {
          try {
            return ctx.slots.register(options, Component)
          } catch (error) {
            warn(`[dsh-ai-passport] 注册槽位 ${slotName} 失败（已降级）：${error?.message ?? error}`)
            return () => {}
          }
        })
      } catch (error) {
        warn(`[dsh-ai-passport] 槽位 ${slotName} 不可用（已降级）：${error?.message ?? error}`)
      }
    }

    function apply(ctx) {
      // 样式只插一次，随 fiber 一起清理（热重载不会堆出一串重复 <style>）。
      try {
        ctx.effect(() => {
          const tag = document.createElement('style')
          tag.dataset.pluginCss = 'dsh-ai-passport'
          tag.textContent = CSS
          document.head.appendChild(tag)
          return () => tag.remove()
        }, 'dsh-ai-passport: styles')
      } catch {
        // 没有 document 的环境（理论上不会发生）就跳过样式
      }

      // 设置侧栏独立一页（docs/09 §5.1）
      installSlot(
        ctx,
        {
          name: SETTINGS_SLOT,
          id: 'ai-passport',
          order: 30,
          label: 'AI Passport',
          inject: () => ({}),
        },
        SettingsPage,
      )

      // 会话标题旁的 list 槽指示器（docs/09 §5.2）——
      // 不再占输入框工具行的 single 槽，官方语音输入插件恢复正常。
      installSlot(
        ctx,
        {
          name: INDICATOR_SLOT,
          id: 'ai-passport-status',
          order: 50,
          label: 'AI Passport 状态',
          inject: () => ({}),
        },
        SessionIndicator,
      )
    }

    // 纯函数一律导出：语音/配对视图的语义由单测直接钉住，设置页/指示器视图同理。
    return {
      apply,
      inject,
      name: 'dsh-ai-passport-client',
      version: VERSION,
      settingsSlot: SETTINGS_SLOT,
      indicatorSlot: INDICATOR_SLOT,
      speechViewFor,
      pairingViewFor,
      settingsViewFor,
      indicatorViewFor,
      normalizeSettings,
      pollIntervalFor,
      focusDelayFor,
    }
  },
})
