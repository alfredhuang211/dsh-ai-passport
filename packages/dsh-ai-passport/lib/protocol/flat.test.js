/**
 * 扁平载荷编解码测试。
 *
 * 重点不是"能跑通"，而是钉住三个会静默出错的点：
 *   1. 字段转义（标题里出现 `|` 或 `\` 时，设备端拆列会错位）；
 *   2. UTF-8 边界截断（切半个汉字 → 设备渲染成乱码/方框）；
 *   3. 与设备端一致的"字段数必须等于 n"（否则设备显示串行）。
 */

import { test } from 'node:test'
import assert from 'node:assert/strict'

import {
  decodeSessionList,
  encodeSessionList,
  encodeTitle,
  sanitizeField,
  truncateUtf8,
  unescapeField,
} from './flat.js'
import { MAX_PAYLOAD_BYTES, SESSION, SESSION_STATE } from './constants.js'

test('字段转义与还原互逆（| 与 \\）', () => {
  const raw = 'a|b\\c\n d'
  const escaped = sanitizeField(raw)
  assert.ok(!escaped.includes('|'), '清洗后不应残留分隔符')
  assert.ok(!escaped.includes('\\'), '清洗后不应残留反斜杠')
  assert.equal(unescapeField(escaped), 'a|b\\c  d')
})

test('标题按 UTF-8 字节截断且不切断多字节字符', () => {
  // "你好世界" = 4 个汉字 × 3B = 12B
  assert.equal(truncateUtf8('你好世界', 7), '你好')
  assert.equal(truncateUtf8('你好世界', 6), '你好')
  assert.equal(truncateUtf8('你好世界', 11), '你好世')
  assert.equal(truncateUtf8('abcdef', 4), 'abcd')
  assert.equal(truncateUtf8('abc', 0), '')
  // encodeTitle 同时做字节上限
  const long = '中'.repeat(100)
  const encoded = encodeTitle(long)
  assert.ok(new TextEncoder().encode(encoded).length <= SESSION.TITLE_BYTES)
})

test('会话快照编码/解码往返一致', () => {
  const sessions = [
    { handle: '01', title: '重构登录模块', state: SESSION_STATE.RUNNING, unread: false },
    { handle: '02', title: '修复导出乱码', state: SESSION_STATE.WAITING, unread: true },
    { handle: '03', title: '调研向量库', state: SESSION_STATE.IDLE, unread: false },
  ]
  const payload = encodeSessionList(sessions, '02')
  assert.equal(payload.n, 3)
  assert.equal(payload.handles, '01|02|03')
  assert.equal(payload.states, 'running|waiting|idle')
  assert.equal(payload.unread, '0|1|0')
  assert.equal(payload.sel, '02')

  const decoded = decodeSessionList(payload)
  assert.deepEqual(decoded.sessions, sessions)
  assert.equal(decoded.sel, '02')
})

test('标题里的竖线与反斜杠不会破坏分列', () => {
  const sessions = [
    { handle: '01', title: 'a|b', state: SESSION_STATE.RUNNING, unread: false },
    { handle: '02', title: 'c\\d', state: SESSION_STATE.IDLE, unread: false },
  ]
  const payload = encodeSessionList(sessions, '')
  assert.equal(payload.titles.split('|').length, 2, '转义后仍应只有 1 个分隔符')
  const decoded = decodeSessionList(payload)
  assert.equal(decoded.sessions[0].title, 'a|b')
  assert.equal(decoded.sessions[1].title, 'c\\d')
})

test('空列表与未选中', () => {
  const payload = encodeSessionList([], '')
  assert.equal(payload.n, 0)
  assert.deepEqual(decodeSessionList(payload), { sessions: [], sel: '' })
})

test('超过上限、非法 handle / 状态都显式报错', () => {
  const many = Array.from({ length: SESSION.MAX + 1 }, (_, i) => ({
    handle: String(i + 1).padStart(2, '0'),
    title: `t${i}`,
    state: SESSION_STATE.IDLE,
    unread: false,
  }))
  assert.throws(() => encodeSessionList(many), RangeError)
  assert.throws(
    () => encodeSessionList([{ handle: '1', title: 'x', state: 'idle' }]),
    TypeError,
  )
  assert.throws(
    () => encodeSessionList([{ handle: '01', title: 'x', state: 'nope' }]),
    TypeError,
  )
})

test('解码时字段数与 n 不一致必须报错（设备会串行）', () => {
  const payload = encodeSessionList([
    { handle: '01', title: 'a', state: 'idle' },
    { handle: '02', title: 'b', state: 'idle' },
  ])
  assert.throws(
    () => decodeSessionList({ ...payload, titles: 'a' }),
    /titles 字段数/,
  )
})

test('最坏 8 条中文标题仍在载荷上限内', () => {
  const sessions = Array.from({ length: SESSION.MAX }, (_, i) => ({
    handle: String(i + 1).padStart(2, '0'),
    // 每条都占满 40B（13 个汉字 = 39B）
    title: '中'.repeat(13),
    state: SESSION_STATE.RUNNING,
    unread: true,
  }))
  const payload = encodeSessionList(sessions, '08')
  const size = new TextEncoder().encode(JSON.stringify(payload)).length
  assert.ok(size <= MAX_PAYLOAD_BYTES, `载荷 ${size}B 应 ≤ ${MAX_PAYLOAD_BYTES}B`)
  // 并确保解码回来标题一个不少
  assert.equal(decodeSessionList(payload).sessions.length, SESSION.MAX)
})
