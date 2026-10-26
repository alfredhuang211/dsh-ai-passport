/**
 * SessionDomain（协议 v2 多会话）单元测试。
 *
 * 用假宿主 + 桩链路覆盖：列表编码/排序、handle 稳定性、未读、选择、子代理过滤、
 * v1 兼容路径、完成消息顺序。这些点在真机上出错都表现为"设备显示不对"，
 * 很难从串口日志直接看出是哪一层的问题。
 */

import { test } from 'node:test'
import assert from 'node:assert/strict'

import { SessionDomain } from './sessions.js'
import { FakeHost } from '../../test/fake-host.js'
import { MSG, SESSION, SESSION_STATE, TASK_STATE } from '../protocol/constants.js'

/** 与 HostLink 同名接口的桩：记录下行消息、可触发设备请求。 */
class StubLink {
  constructor({ protocolVersion = 2, connected = true, handshake = true } = {}) {
    this.sent = []
    this.handlers = new Map()
    this.settings = { protocolVersion, connected, handshake }
  }

  onMessage(type, handler) {
    this.handlers.set(type, handler)
    return () => this.handlers.delete(type)
  }

  send(type, payload) {
    this.sent.push({ type, payload })
    return Promise.resolve()
  }

  snapshot() {
    return {
      state: this.settings.connected ? 'connected' : 'idle',
      handshakeComplete: this.settings.handshake,
      deviceInfo: { protocolVersion: this.settings.protocolVersion },
    }
  }

  invoke(type, payload) {
    const handler = this.handlers.get(type)
    assert.ok(handler, `没有注册 ${type} 处理器`)
    return handler({ type, ...payload })
  }

  ofType(type) {
    return this.sent.filter((entry) => entry.type === type)
  }
}

const tick = () => new Promise((resolve) => setTimeout(resolve, 5))

function summary(id, { title, updatedAt = 0, running = false, origin, blank = false } = {}) {
  return {
    sessionId: id,
    updatedAt,
    running,
    blank,
    origin,
    projections: { values: { title } },
  }
}

async function setup(options = {}) {
  const host = new FakeHost()
  const link = new StubLink(options)
  const domainState = {}
  const domain = new SessionDomain({ link, logger: () => {}, domainState })
  domain.attach({}, host)
  await tick()
  return { host, link, domain, domainState }
}

test('连接后先发一份空列表（n=0）', async () => {
  const { link, domain } = await setup()
  const lists = link.ofType(MSG.SESSION_LIST)
  assert.ok(lists.length >= 1, '应至少推一次 session.list')
  assert.equal(lists[0].payload.n, 0)
  assert.equal(lists[0].payload.handles, '')
  domain.dispose()
})

test('列表按 updatedAt 倒序，最新的在最上', async () => {
  const { host, link, domain } = await setup()
  host.emit('api-session/added', summary('a', { title: 'A', updatedAt: 100 }))
  host.emit('api-session/added', summary('b', { title: 'B', updatedAt: 300 }))
  host.emit('api-session/added', summary('c', { title: 'C', updatedAt: 200 }))
  domain.flush()

  const payload = link.ofType(MSG.SESSION_LIST).at(-1).payload
  assert.deepEqual(payload.titles.split('|'), ['B', 'C', 'A'])
  assert.deepEqual(payload.states.split('|'), ['idle', 'idle', 'idle'])
  domain.dispose()
})

test('子代理会话与空白会话不上设备（运行中的空白保留）', async () => {
  const { host, link, domain } = await setup()
  host.emit('api-session/added', summary('main', { title: '主任务', updatedAt: 100 }))
  host.emit('api-session/added', summary('sub', { title: '子代理', updatedAt: 200, origin: 'subagent' }))
  host.emit('api-session/added', summary('blank', { title: '空白', updatedAt: 300, blank: true }))
  domain.flush()

  const payload = link.ofType(MSG.SESSION_LIST).at(-1).payload
  assert.equal(payload.n, 1)
  assert.equal(payload.titles, '主任务')
  domain.dispose()
})

test('turn 结束：未读置位；设备选中后清除并回发详情', async () => {
  const { host, link, domain } = await setup()
  host.emit('api-session/added', summary('a', { title: '重构登录', updatedAt: 100 }))
  host.startTurn('a')
  await tick()
  host.endTurn('a', { kind: 'completed' })
  await tick()

  let payload = link.ofType(MSG.SESSION_LIST).at(-1).payload
  assert.equal(payload.unread, '1', '未选中时结束后应置未读')
  assert.equal(payload.states, 'idle')

  const handle = payload.handles
  assert.match(handle, /^[0-9a-f]{2}$/)
  link.invoke(MSG.SESSION_SELECT, { h: handle })
  domain.flush()

  payload = link.ofType(MSG.SESSION_LIST).at(-1).payload
  assert.equal(payload.sel, handle, '选中应回显')
  assert.equal(payload.unread, '0', '进入会话应清未读')
  assert.ok(link.ofType(MSG.SESSION_DETAIL).length >= 1, '应下发 session.detail')

  const detail = link.ofType(MSG.SESSION_DETAIL).at(-1).payload
  assert.equal(detail.h, handle)
  assert.equal(detail.title, '重构登录')
  domain.dispose()
})

test('handle 在同一连接内稳定，链路就绪后重新分配', async () => {
  const { host, link, domain } = await setup()
  host.emit('api-session/added', summary('a', { title: 'A', updatedAt: 100 }))
  host.emit('api-session/added', summary('b', { title: 'B', updatedAt: 200 }))
  domain.flush()
  const first = link.ofType(MSG.SESSION_LIST).at(-1).payload
  const handleA = first.handles.split('|')[first.titles.split('|').indexOf('A')]

  host.emit('api-session/added', summary('c', { title: 'C', updatedAt: 300 }))
  domain.flush()
  const second = link.ofType(MSG.SESSION_LIST).at(-1).payload
  assert.equal(second.handles.split('|')[second.titles.split('|').indexOf('A')], handleA, 'handle 不应变化')

  domain.onLinkReady()
  const third = link.ofType(MSG.SESSION_LIST).at(-1).payload
  assert.equal(third.sel, '', '重连后选中应清空')
  assert.deepEqual(third.handles.split('|').sort(), ['01', '02', '03'], '重连后从 01 重新分配')
  domain.dispose()
})

test('v1 旧固件走 task.state 兼容路径', async () => {
  const { host, link, domain } = await setup({ protocolVersion: 1 })
  host.emit('api-session/added', summary('a', { title: 'A', updatedAt: 100 }))
  host.startTurn('a')
  domain.flush()

  assert.equal(link.ofType(MSG.SESSION_LIST).length, 0, 'v1 不应收到 session.list')
  const states = link.ofType(MSG.TASK_STATE)
  assert.ok(states.length >= 1)
  assert.equal(states.at(-1).payload.state, TASK_STATE.RUNNING)

  host.endTurn('a', { kind: 'completed' })
  domain.flush()
  assert.equal(link.ofType(MSG.TASK_STATE).at(-1).payload.state, TASK_STATE.COMPLETED)
  assert.match(link.ofType(MSG.TASK_STATE).at(-1).payload.title, /A/)
  domain.dispose()
})

test('turn 结束先发列表再发 session.done，结果类型正确', async () => {
  const { host, link, domain } = await setup()
  host.emit('api-session/added', summary('a', { title: 'A', updatedAt: 100 }))
  host.startTurn('a')
  await tick()
  const before = link.sent.length
  host.endTurn('a', { kind: 'error' })
  await tick()

  const after = link.sent.slice(before).filter((e) => e.type === MSG.SESSION_LIST || e.type === MSG.SESSION_DONE)
  assert.deepEqual(after.map((e) => e.type), [MSG.SESSION_LIST, MSG.SESSION_DONE], '顺序必须是列表在前')
  assert.equal(after[1].payload.result, 'error')
  assert.equal(link.ofType(MSG.SESSION_LIST).at(-1).payload.states, 'error')
  domain.dispose()
})

test('设备请求不存在的 handle 时回 toast，不崩溃', async () => {
  const { link, domain } = await setup()
  link.invoke(MSG.SESSION_SELECT, { h: 'ff' })
  const toasts = link.ofType(MSG.TOAST)
  assert.equal(toasts.length, 1)
  assert.match(toasts[0].payload.text, /会话已关闭/)
  domain.dispose()
})

test('录音期间抑制推送，结束后补推', async () => {
  const { host, link, domain } = await setup()
  domain.setDeviceRecording(true)
  const before = link.ofType(MSG.SESSION_LIST).length
  host.emit('api-session/added', summary('a', { title: 'A', updatedAt: 100 }))
  domain.flush()
  assert.equal(link.ofType(MSG.SESSION_LIST).length, before, '录音期间不应推送')

  domain.setDeviceRecording(false)
  domain.flush()
  assert.ok(link.ofType(MSG.SESSION_LIST).length > before, '录音结束应补推')
  domain.dispose()
})

test('快照同时给出旧四态与 v2 会话数组', async () => {
  const { host, domain, domainState } = await setup()
  host.emit('api-session/added', summary('a', { title: 'A', updatedAt: 100 }))
  host.startTurn('a')
  domain.flush()

  const snapshot = domain.snapshot()
  assert.equal(snapshot.state, TASK_STATE.RUNNING)
  assert.equal(snapshot.sessions.length, 1)
  assert.equal(snapshot.sessions[0].title, 'A')
  assert.equal(snapshot.sessions[0].state, SESSION_STATE.RUNNING)
  assert.ok(domainState.sessions.length === 1)
  assert.ok(SESSION.MAX >= snapshot.sessions.length)
  domain.dispose()
})
