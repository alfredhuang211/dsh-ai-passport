/**
 * 客户端 bundle 的离线校验（S4：设置页 + 会话旁指示器）。
 *
 * 为什么必须专门测这一层：客户端半区是**预先构建的浏览器 bundle**，由 DSH 的
 * `client-modules` 直接拼接下发。它的错误只在 DSH 启动时才暴露，而且一旦出错
 * （包名对不上、导出的东西不对、require 了不存在的模块、注册了错误的槽位）
 * 会让整个 client 包组合失败，表现为"客户端整个起不来"——排查成本极高。
 *
 * 这个测试用一个假的 `window.__ModuleLoader__`、假的 `require` 与一个迷你渲染器
 * 把 bundle 跑一遍，把上面那些问题在毫秒级、无副作用地抓出来。
 */

import { test } from 'node:test'
import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const PACKAGE_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const CLIENT_BUNDLE = path.join(PACKAGE_ROOT, 'lib', 'client.js')
const PACKAGE_NAME = 'dsh-ai-passport'

const SETTINGS_SLOT = 'settings.section'
const INDICATOR_SLOT = 'conversation.session.header.utilities'

/**
 * 一个够用的 React 替身。
 *
 * `createElement` 按 React 语义把 children 放进 `props.children`（组件里
 * `Row({ label, children })` 这类写法依赖它）；hooks 全部是 no-op —— 组件体
 * 能跑、能返回元素树就够了，轮询/副作用另有纯函数覆盖。
 */
const fakeReact = {
  createElement: (type, props, ...children) => {
    const nextProps = { ...(props ?? {}) }
    if (children.length === 1) nextProps.children = children[0]
    else if (children.length > 1) nextProps.children = children
    return { type, props: nextProps, children }
  },
  useState: (initial) => [typeof initial === 'function' ? initial() : initial, () => {}],
  useEffect: () => {},
  useRef: (initial) => ({ current: initial }),
  useCallback: (fn) => fn,
  Fragment: Symbol('Fragment'),
}

function createFakeDocument() {
  return {
    createElement: () => ({ dataset: {}, textContent: '', remove() {} }),
    head: { appendChild() {} },
    addEventListener() {},
    removeEventListener() {},
    hidden: false,
  }
}

/**
 * 执行一次 bundle 源码，拿回 `definition` 与 factory 的导出。
 * bundle 在模块顶层只做一次 `__ModuleLoader__.load`，没有其它副作用。
 */
async function loadBundle() {
  const source = await readFile(CLIENT_BUNDLE, 'utf8')
  const required = []
  let definition = null
  let exports = null

  const loader = {
    load(def) {
      if (definition) throw new Error('bundle 重复注册（必须恰好 load 一次）')
      definition = def
      const require = (name) => {
        required.push(name)
        if (name === 'react') return fakeReact
        if (name === 'react/jsx-runtime') {
          return { jsx: (type, props) => ({ type, props }), jsxs: (type, props) => ({ type, props }) }
        }
        if (name === '@deepseek-ai/dsh-client-ui-primitives') {
          return new Proxy({}, { get: () => () => null })
        }
        throw new Error(`客户端 bundle require 了未提供的模块：${name}`)
      }
      exports = def.factory(require)
      return exports
    },
  }

  const previousWindow = globalThis.window
  globalThis.window = { __ModuleLoader__: loader }
  try {
    // bundle 是普通脚本（非 ESM），用间接 eval 在全局作用域执行
    // eslint-disable-next-line no-eval
    ;(0, eval)(source)
  } finally {
    globalThis.window = previousWindow
  }

  assert.ok(definition, 'bundle 必须调用 window.__ModuleLoader__.load')
  return { definition, exports, required, source }
}

/** 假的客户端 ctx：记录 effect 与槽位注册，供断言。 */
function createCtx({ failSlots = false } = {}) {
  const effects = []
  const slotInjections = []
  const calls = []
  const ctx = {
    effect: (callback, label) => {
      const cleanup = callback()
      effects.push({ label: label ?? '(无标签)', cleanup: typeof cleanup === 'function' ? cleanup : null })
      return () => {}
    },
    slots: {
      inject: (key, callback) => {
        // 模拟"当前 DSH 版本没有这个槽位/服务"：slots.inject 直接抛错
        if (failSlots) throw new Error('slots 服务不可用')
        slotInjections.push({ key, registrations: [] })
        const dispose = callback()
        return typeof dispose === 'function' ? dispose : () => {}
      },
      register: (options, component) => {
        const last = slotInjections[slotInjections.length - 1]
        last?.registrations.push({ options, component })
        calls.push({ kind: 'register', options, component })
        return () => {}
      },
    },
  }
  return { ctx, effects, slotInjections, calls }
}

async function runClientBundle(options = {}) {
  const loaded = await loadBundle()
  const harness = createCtx(options)
  const previousDocument = globalThis.document
  // document 必须留到 apply() 跑完：`apply()` 会往 document.head 插样式。
  globalThis.document = createFakeDocument()
  try {
    loaded.exports.apply(harness.ctx)
  } finally {
    globalThis.document = previousDocument
  }
  return { ...loaded, ...harness }
}

function registrationsOf(slotInjections, slotName) {
  return slotInjections
    .filter((injection) => injection.key === slotName)
    .flatMap((injection) => injection.registrations)
}

/** 迷你渲染器：把元素树里 function component 逐个执行掉（hooks 都是 no-op）。 */
function renderTree(node) {
  if (node == null || typeof node === 'boolean') return null
  if (typeof node === 'string' || typeof node === 'number') return String(node)
  if (Array.isArray(node)) return node.map(renderTree)
  if (typeof node !== 'object') return null
  if (typeof node.type === 'function') return renderTree(node.type(node.props ?? {}))
  return { ...node, children: renderTree(node.children) }
}

function textOf(node) {
  if (node == null) return ''
  if (typeof node === 'string') return node
  if (typeof node === 'number') return String(node)
  if (Array.isArray(node)) return node.map(textOf).join('')
  if (typeof node === 'object') return textOf(node.children)
  return ''
}

function findAll(node, predicate, out = []) {
  if (node == null || typeof node !== 'object') return out
  if (Array.isArray(node)) {
    for (const child of node) findAll(child, predicate, out)
    return out
  }
  if (predicate(node)) out.push(node)
  findAll(node.children, predicate, out)
  return out
}

/** 临时替换 globalThis.fetch，记录请求并回一个固定响应。 */
function stubFetch(handler) {
  const previous = globalThis.fetch
  const calls = []
  globalThis.fetch = async (url, init) => {
    calls.push({ url: String(url), init })
    const payload = await handler?.(String(url), init)
    const body = payload ?? { ok: true, result: { message: '已保存并生效' } }
    return { ok: true, json: async () => body }
  }
  return {
    calls,
    restore() {
      if (previous === undefined) delete globalThis.fetch
      else globalThis.fetch = previous
    },
  }
}

/** 一份"设备已连上、有会话、有设置"的完整快照，供渲染测试用。 */
function makeFullState() {
  return {
    link: {
      state: 'connected',
      device: { name: 'AI Passport-8F2C', rssi: -52 },
      deviceInfo: { firmware: '1.4.2', protocolVersion: 2, batteryPercent: 87 },
      handshakeComplete: true,
      lastError: null,
    },
    transport: 'bridge',
    diagnose: { adapterState: 'poweredOn', childRunning: true, noble: 'loaded' },
    sessions: [
      { sessionId: 's1', handle: '01', title: '修复登录问题', state: 'running', unread: false, updatedAt: 1 },
      { sessionId: 's2', handle: '02', title: '整理周报', state: 'idle', unread: true, updatedAt: 2 },
    ],
    selectedHandle: '01',
    deviceSelectedSessionId: 's1',
    desktopFocusedSessionId: 's2',
    settings: {
      maskTitles: true,
      soundEnabled: false,
      backlightPulse: true,
      detailSummary: false,
      detailElapsed: true,
      voiceAutoSend: true,
      maxSessions: 5,
    },
    speech: { ready: false, needsPreparation: true, providerId: 'sensevoice-local', preparing: false },
    speechError: null,
    pairing: { needed: true, paired: false, deviceId: null, error: null, remaining: 3 },
    config: { autoConnect: false, autoReconnect: true, targetDeviceId: '', voice: { providerId: '' } },
  }
}

// ────────────────────────────── bundle 契约 ──────────────────────────────

test('bundle 用 __ModuleLoader__.load 注册，且 id 严格等于包名', async () => {
  const { definition } = await loadBundle()
  // id 与包名不一致会被 client-modules 组合成一个启动期错误，这里必须卡死
  assert.equal(definition.id, PACKAGE_NAME)
  assert.equal(typeof definition.factory, 'function')
})

test('bundle 不 require 任何未提供的模块', async () => {
  const { required } = await runClientBundle()
  // 只允许这两个（这也是其它官方客户端 bundle 实际用到的）
  const allowed = new Set(['react', 'react/jsx-runtime', '@deepseek-ai/dsh-client-ui-primitives'])
  for (const name of required) assert.ok(allowed.has(name), `不允许 require ${name}`)
})

test('导出 apply 与 inject，inject 只声明 slots', async () => {
  const { exports } = await runClientBundle()
  assert.equal(typeof exports.apply, 'function')
  assert.ok(Array.isArray(exports.inject))
  assert.deepEqual(exports.inject, ['slots'])
})

test('导出纯函数视图与轮询/节流决策（单测直接钉住语义）', async () => {
  const { exports } = await runClientBundle()
  for (const name of ['speechViewFor', 'pairingViewFor', 'settingsViewFor', 'indicatorViewFor', 'normalizeSettings']) {
    assert.equal(typeof exports[name], 'function', `缺少纯函数 ${name}`)
  }
  assert.equal(typeof exports.pollIntervalFor, 'function')
  assert.equal(typeof exports.focusDelayFor, 'function')
})

// ─────────────────────────────── 槽位注册 ───────────────────────────────

test('apply() 在 settings.section 注册了设置页（id/order/label 齐备）', async () => {
  const { slotInjections, calls } = await runClientBundle()
  const registrations = registrationsOf(slotInjections, SETTINGS_SLOT)
  assert.equal(registrations.length, 1, '必须恰好注册一个设置分区')

  const { options, component } = registrations[0]
  assert.equal(options.name, SETTINGS_SLOT)
  assert.equal(options.id, 'ai-passport')
  assert.equal(options.order, 30)
  assert.equal(options.label, 'AI Passport')
  assert.equal(typeof options.inject, 'function', '注册参数按契约带 inject')
  // 没有 locale 服务时声明 locale 会 fail-loud，这里锁死"不要声明"
  assert.equal('locale' in options, false, '不允许声明 locale（本包没有字典服务）')
  assert.equal(typeof component, 'function', '组件必须是可渲染的 React 组件')

  const registration = calls.find((call) => call.kind === 'register' && call.options.name === SETTINGS_SLOT)
  assert.ok(registration)
})

test('apply() 在 conversation.session.header.utilities 注册了会话旁指示器（list 槽）', async () => {
  const { slotInjections } = await runClientBundle()
  const registrations = registrationsOf(slotInjections, INDICATOR_SLOT)
  assert.equal(registrations.length, 1, '必须恰好注册一个指示器')

  const { options, component } = registrations[0]
  assert.equal(options.name, INDICATOR_SLOT)
  assert.equal(options.id, 'ai-passport-status', 'list 槽的 id 必填且稳定')
  assert.equal(typeof options.order, 'number')
  assert.equal(typeof component, 'function')
})

test('apply() 只注册这两个槽位：旧输入框挂件槽不再出现', async () => {
  const { slotInjections } = await runClientBundle()
  const keys = slotInjections.map((injection) => injection.key)
  assert.deepEqual(keys, [SETTINGS_SLOT, INDICATOR_SLOT])
  assert.ok(!keys.includes('conversation.input.activity'), 'single 槽会挤坏官方语音输入插件，必须移除')
})

test('槽位/服务缺失时 apply() 降级而不是抛错（不阻塞其它功能）', async () => {
  // 模拟一个没有 slots 服务、或槽位未声明的 DSH 版本
  await assert.doesNotReject(() => runClientBundle({ failSlots: true }))
})

test('apply() 注册样式，且样式 effect 返回清理函数（不留孤儿 style 标签）', async () => {
  const { effects } = await runClientBundle()
  assert.equal(effects.length, 1, '除样式外不应再占用其它 effect')

  const styleEffect = effects.find((entry) => entry.label === 'dsh-ai-passport: styles')
  assert.ok(styleEffect, `缺少样式 effect，实际：${effects.map((entry) => entry.label).join(', ')}`)
  assert.equal(typeof styleEffect.cleanup, 'function', '样式 effect 必须返回清理函数')
  assert.doesNotThrow(() => styleEffect.cleanup())
})

// ───────────────────────────── 设置页视图模型 ─────────────────────────────

test('settingsViewFor：空状态不崩，给出默认值', async () => {
  const { exports } = await runClientBundle()
  const view = exports.settingsViewFor(null)
  assert.equal(view.connected, false)
  assert.equal(view.linkStateText, '未连接')
  assert.equal(view.settings.maskTitles, false)
  assert.equal(view.settings.soundEnabled, true, '提示音默认开')
  assert.equal(view.settings.maxSessions, 8)
  assert.equal(view.version, exports.version)
  assert.equal(view.pairing.show, false, '没有 pairing 字段就不显示配对区')
})

test('settingsViewFor：完整状态映射设备信息/会话标题/设置', async () => {
  const { exports } = await runClientBundle()
  const view = exports.settingsViewFor(makeFullState())
  assert.equal(view.connected, true)
  assert.equal(view.linkStateText, '已连接')
  assert.equal(view.deviceName, 'AI Passport-8F2C')
  assert.equal(view.firmware, '1.4.2')
  assert.equal(view.batteryPercent, 87)
  assert.equal(view.rssi, -52)
  assert.equal(view.protocolVersion, 2)
  assert.equal(view.handshakeComplete, true)
  assert.equal(view.transport, 'bridge')
  assert.equal(view.settings.maskTitles, true)
  assert.equal(view.settings.maxSessions, 5)
  assert.equal(view.settings.voiceAutoSend, true)
  assert.equal(view.pairing.show, true)
  assert.equal(view.pairing.paired, false)
  assert.equal(view.deviceSelectedSessionId, 's1')
  assert.equal(view.deviceSelectedSessionTitle, '修复登录问题')
  assert.equal(view.focusedSessionTitle, '整理周报')
  assert.equal(view.sessionCount, 2)
})

test('normalizeSettings：坏值忽略、maxSessions 钳到 1–8', async () => {
  const { exports } = await runClientBundle()
  const view = exports.normalizeSettings({
    maskTitles: 'yes',
    soundEnabled: false,
    maxSessions: 99,
    detailElapsed: 0,
    unknownKey: true,
  })
  assert.equal(view.maskTitles, false, '非布尔值必须忽略、回落到默认值')
  assert.equal(view.soundEnabled, false)
  assert.equal(view.maxSessions, 8)
  assert.equal(view.detailElapsed, true)
  assert.equal('unknownKey' in view, false, '未知字段不往下游传')

  assert.equal(exports.normalizeSettings({ maxSessions: 0 }).maxSessions, 1)
  assert.equal(exports.normalizeSettings({ maxSessions: 4 }).maxSessions, 4)
  assert.equal(exports.normalizeSettings(undefined).maxSessions, 8)
})

test('轮询与节流决策：2s/10s、1s 节流（纯函数）', async () => {
  const { exports } = await runClientBundle()
  assert.equal(exports.pollIntervalFor(false), 2000)
  assert.equal(exports.pollIntervalFor(true), 10000, '标签页隐藏时降频到 10s')

  assert.equal(exports.focusDelayFor(0, 0), 1000, '首次上报不等待')
  assert.equal(exports.focusDelayFor(0, 1200), 0)
  assert.equal(exports.focusDelayFor(1000, 1500), 500)
  assert.equal(exports.focusDelayFor(1000, 2000), 0)
})

// ──────────────────────────── 设置页渲染 ────────────────────────────

test('设置页组件在空状态（首屏 state=null）也能渲染', async () => {
  const { slotInjections } = await runClientBundle()
  const component = registrationsOf(slotInjections, SETTINGS_SLOT)[0].component
  let tree = null
  assert.doesNotThrow(() => {
    tree = renderTree(component({}))
  })
  const text = textOf(tree)
  assert.match(text, /连接/)
  assert.match(text, /扫描并连接/)
  assert.match(text, /未选择，在设备上用上下键选择/)
  assert.match(text, /关于/)
})

test('设置页组件在完整状态下渲染出全部信息架构（§5.1）', async () => {
  const { slotInjections } = await runClientBundle()
  const component = registrationsOf(slotInjections, SETTINGS_SLOT)[0].component
  const tree = renderTree(component({ stateOverride: makeFullState() }))
  const text = textOf(tree)

  // 6 个分区
  for (const section of ['连接', '配对', '设备显示', '语音', '会话接入', '关于']) {
    assert.match(text, new RegExp(section), `设置页缺少分区：${section}`)
  }
  // 连接
  assert.match(text, /AI Passport-8F2C/)
  assert.match(text, /1\.4\.2/)
  assert.match(text, /87%/)
  assert.match(text, /-52 dBm/)
  assert.match(text, /断开/)
  assert.match(text, /诊断蓝牙/)
  // 配对
  assert.match(text, /6 位配对码/)
  // 设备显示
  for (const label of ['标题打码', '提示音', '背光脉冲', '最多会话', '详情·最近输出', '详情·耗时', '提示音试听']) {
    assert.match(text, new RegExp(label), `设备显示缺少：${label}`)
  }
  for (const sound of ['待审批', '会话完成', '出错', '已选择']) {
    assert.match(text, new RegExp(sound))
  }
  // 语音
  assert.match(text, /未启用/)
  assert.match(text, /准备模型/)
  assert.match(text, /自动发送/)
  // 会话接入
  assert.match(text, /修复登录问题/)
  assert.match(text, /整理周报/)
  // 关于
  assert.match(text, /插件版本/)
  assert.match(text, /1\.2\.0/)
  assert.match(text, /MIT/)
})

test('设置页开关会发 settings.set，payload 是 {patch:{...}}', async () => {
  const { exports, slotInjections } = await runClientBundle()
  const component = registrationsOf(slotInjections, SETTINGS_SLOT)[0].component
  const tree = renderTree(component({ stateOverride: makeFullState() }))
  const toggle = findAll(tree, (node) => node.props?.['aria-label'] === '标题打码')[0]
  assert.ok(toggle, '找不到「标题打码」开关')

  const fetchStub = stubFetch(async () => ({
    ok: true,
    result: { message: '已保存并生效', settings: { ...makeFullState().settings, maskTitles: false } },
  }))
  try {
    await toggle.props.onClick()
  } finally {
    fetchStub.restore()
  }

  assert.equal(fetchStub.calls.length, 1)
  assert.equal(fetchStub.calls[0].url, '/dsh-passport/action')
  const body = JSON.parse(fetchStub.calls[0].init.body)
  assert.equal(body.action, 'settings.set')
  assert.deepEqual(body.payload, { patch: { maskTitles: false } }, '完整状态里 maskTitles=true，点一下应写 false')
  assert.equal(exports.version, '1.2.0')
})

test('设置页试听按钮发 sound.test（kind 与宿主 SOUND_KIND 对齐）', async () => {
  const { slotInjections } = await runClientBundle()
  const component = registrationsOf(slotInjections, SETTINGS_SLOT)[0].component
  const tree = renderTree(component({ stateOverride: makeFullState() }))
  const button = findAll(tree, (node) => node.type === 'button' && textOf(node) === '待审批')[0]
  assert.ok(button, '找不到「待审批」试听按钮')

  const fetchStub = stubFetch(async () => ({ ok: true, result: { message: '已发送试听' } }))
  try {
    await button.props.onClick()
  } finally {
    fetchStub.restore()
  }

  const body = JSON.parse(fetchStub.calls[0].init.body)
  assert.equal(body.action, 'sound.test')
  assert.deepEqual(body.payload, { kind: 'approve' })
})

test('设置页配对码输入走 pair.submit / pair.unpair（回归钉子）', async () => {
  const { slotInjections, source } = await runClientBundle()
  assert.match(source, /pair\.submit/, '缺少提交配对码的动作')
  assert.match(source, /pair\.unpair/, '缺少解除配对的动作')
  assert.match(source, /ap-input/, '缺少配对码输入框样式')
  // 配对状态变化时清掉过渡提示（不再挂着"等待设备确认…"）
  assert.match(source, /\[pairingKey\]/, '配对状态变化必须清掉 notice')

  const component = registrationsOf(slotInjections, SETTINGS_SLOT)[0].component
  const tree = renderTree(component({ stateOverride: makeFullState() }))
  const input = findAll(tree, (node) => node.type === 'input')[0]
  assert.ok(input, '未配对时必须给 6 位码输入框')
  assert.equal(input.props.maxLength, 6)
})

// ───────────────────────── 会话旁指示器（三态） ─────────────────────────

test('indicatorViewFor：三态文案与颜色（灰/蓝/绿）', async () => {
  const { exports } = await runClientBundle()

  const offline = exports.indicatorViewFor({ link: { state: 'idle' } }, 's1')
  assert.equal(offline.tone, 'offline')
  assert.equal(offline.text, 'AI Passport 未连接')
  assert.match(offline.title, /未连接/)

  const connected = exports.indicatorViewFor(
    {
      link: { state: 'connected', device: { name: 'AI Passport-8F2C' }, deviceInfo: { batteryPercent: 87, protocolVersion: 2 } },
      deviceSelectedSessionId: 's2',
    },
    's1',
  )
  assert.equal(connected.tone, 'connected')
  assert.equal(connected.text, '设备已连接')
  // 悬停 title 显示设备名/电量/协议版本
  assert.match(connected.title, /AI Passport-8F2C/)
  assert.match(connected.title, /电量 87%/)
  assert.match(connected.title, /协议 v2/)

  const active = exports.indicatorViewFor(
    { link: { state: 'connected', device: { name: 'AI Passport-8F2C' } }, deviceSelectedSessionId: 's1' },
    's1',
  )
  assert.equal(active.tone, 'active')
  assert.equal(active.text, '本会话已在设备上')

  // 设备没选任何会话时，任何窗口都属于"设备已连接"
  const none = exports.indicatorViewFor(
    { link: { state: 'connected', device: { name: 'AI Passport-8F2C' } }, deviceSelectedSessionId: '' },
    's1',
  )
  assert.equal(none.tone, 'connected')
})

test('指示器组件在空状态/完整状态下都能渲染', async () => {
  const { slotInjections } = await runClientBundle()
  const component = registrationsOf(slotInjections, INDICATOR_SLOT)[0].component

  let emptyTree = null
  assert.doesNotThrow(() => {
    emptyTree = renderTree(component({ sessionId: 's1' }))
  })
  assert.equal(emptyTree.props['data-tone'], 'offline')
  assert.equal(textOf(emptyTree), 'AI Passport 未连接')

  const fullTree = renderTree(component({ sessionId: 's1', stateOverride: makeFullState() }))
  assert.equal(fullTree.props['data-tone'], 'active')
  assert.equal(textOf(fullTree), '本会话已在设备上')
  assert.equal(fullTree.props.role, 'status')
})

test('指示器挂载/变化时会上报 session.focus（源码级钉子 + 节流纯函数）', async () => {
  const { source, exports } = await runClientBundle()
  assert.match(source, /act\('session\.focus', \{ sessionId/, '缺少 session.focus 上报')
  assert.match(source, /useSessionFocus/, '缺少聚焦会话上报 hook')
  // 节流窗口是 1s：同一窗口快速切换会话不会把宿主刷爆
  assert.equal(exports.focusDelayFor(0, 0), 1000)
})

// ──────────────────────────── 源码级回归钉子 ────────────────────────────

test('旧版挂件/旧弹窗的痕迹已清除（防止回退到占 single 槽的实现）', async () => {
  const { source } = await runClientBundle()
  for (const gone of [
    'conversation.input.activity',
    'ap-widget',
    'ap-pill',
    'ap-pop',
    'PassportWidget',
    'inputActions',
    'voice.take',
    'voice.ack',
  ]) {
    assert.ok(!source.includes(gone), `客户端 bundle 里不应再出现 ${gone}`)
  }
  // 新契约必须都在
  for (const present of [
    'settings.section',
    'conversation.session.header.utilities',
    'settings.set',
    'sound.test',
    'session.focus',
    'ctx.effect',
    'dsh-ai-passport: styles',
  ]) {
    assert.ok(source.includes(present), `客户端 bundle 缺少 ${present}`)
  }
})

test('设置保存失败会显式报错（不静默）', async () => {
  const { source } = await runClientBundle()
  assert.match(source, /已保存并生效/, '成功时必须给「已保存并生效」反馈')
  assert.match(source, /请求失败/, '失败时必须给出可读错误，不能静默')
  assert.match(source, /宿主未响应/, '宿主没响应时也要有明确文案')
})

// ─────────────────────────── 语音/配对视图语义 ───────────────────────────

test('语音区：已启用时不显示「准备模型」按钮', async () => {
  const { exports } = await runClientBundle()
  const view = exports.speechViewFor({ speech: { ready: true, providerId: 'sensevoice', providerName: 'SenseVoice' } })
  assert.equal(view.text, '已启用')
  assert.equal(view.showPrepare, false, '已启用就不该再给"准备模型"')
})

test('语音区：有识别提供者但未就绪 → 显示「准备模型」', async () => {
  const { exports } = await runClientBundle()
  const view = exports.speechViewFor({ speech: { ready: false, providerId: 'sensevoice', detail: '下载中 20%' } })
  assert.equal(view.text, '未启用')
  assert.equal(view.showPrepare, true)
})

test('语音区：standby（已装未预热）显示已启用、不给按钮', async () => {
  const { exports } = await runClientBundle()
  const view = exports.speechViewFor({
    speech: { ready: true, phase: 'standby', providerId: 'sensevoice-local', needsPreparation: false },
  })
  assert.equal(view.text, '已启用')
  assert.equal(view.showPrepare, false)
})

test('语音区：正在下载/加载时显示进度说明，而不是「准备模型」', async () => {
  const { exports } = await runClientBundle()
  const view = exports.speechViewFor({
    speech: { ready: false, providerId: 'sensevoice-local', preparing: true, needsPreparation: false, detail: '正在下载模型 120/240 MB' },
  })
  assert.equal(view.showPrepare, false, '下载中不该再给按钮')
  assert.match(view.hint ?? '', /正在下载模型/)
})

test('语音区：没有识别提供者时不显示「准备模型」（prepare 必然抛错）', async () => {
  const { exports } = await runClientBundle()
  const view = exports.speechViewFor({
    speech: { ready: false, providerId: null },
    speechError: '没有可用的识别提供者。请在 DSH 里启用本地语音识别（SenseVoice）或安装一个识别插件。',
  })
  assert.equal(view.showPrepare, false, '没有提供者时不给做不到的按钮')
  assert.match(view.hint ?? '', /没有可用的识别提供者/)
})

test('语音区：宿主没有语音服务时也只说明、不给按钮', async () => {
  const { exports } = await runClientBundle()
  const view = exports.speechViewFor({ speech: null, speechError: '当前 DSH 没有启用语音识别服务（ctx.speechToText 不存在）' })
  assert.equal(view.text, '未启用')
  assert.equal(view.showPrepare, false)
  assert.ok(view.hint)
})

test('配对视图：未配对给输入入口、已配对给解除按钮、失败文案可读', async () => {
  const { exports } = await runClientBundle()
  const view = exports.pairingViewFor

  assert.equal(view(null).show, false, '没有配对状态就不显示这一区')

  const needed = view({ pairing: { needed: true, paired: false, deviceId: 'a1b2c3', error: null } })
  assert.equal(needed.show, true)
  assert.equal(needed.needed, true)
  assert.equal(needed.error, null)

  const limited = view({ pairing: { needed: true, paired: false, error: 'rate-limited', remaining: 0 } })
  assert.match(limited.error, /一分钟/)
  assert.equal(limited.remaining, 0)

  const badCode = view({ pairing: { needed: true, paired: false, error: 'bad-code' } })
  assert.match(badCode.error, /配对码不对/)

  const paired = view({ pairing: { needed: false, paired: true, deviceId: 'a1b2c3' } })
  assert.equal(paired.paired, true)
  assert.equal(paired.deviceId, 'a1b2c3')
})

// ───────────────────────────── package 契约 ─────────────────────────────

test('package.json 的 dsh.client 与 exports["./client"] 都已声明，版本与 bundle 对齐', async () => {
  const { exports } = await runClientBundle()
  const pkg = JSON.parse(await readFile(path.join(PACKAGE_ROOT, 'package.json'), 'utf8'))
  assert.ok(pkg.dsh?.client, '必须声明 dsh.client，否则 client-modules 不扫描')
  assert.equal(pkg.dsh.client.platform, 'web')
  assert.ok(Array.isArray(pkg.dsh.client.inject))
  // client-modules 通过 exports["./client"] 定位 bundle，缺失会让整个组合失败
  const clientExport = pkg.exports?.['./client']
  const resolved = typeof clientExport === 'string' ? clientExport : clientExport?.default
  assert.equal(resolved, './lib/client.js')

  // 声明的路径必须真实存在
  const target = path.join(PACKAGE_ROOT, resolved)
  await assert.doesNotReject(() => readFile(target), `客户端 bundle 不存在：${target}`)

  // 版本号在 关于 页展示；两处漂移会在发布时坑人，这里钉住
  assert.equal(exports.version, pkg.version, '客户端 VERSION 必须与 package.json 一致')
})
