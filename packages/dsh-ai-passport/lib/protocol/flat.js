/**
 * 协议 v2 的「扁平载荷」编解码。
 *
 * 为什么需要它：设备端的 JSON 取值器（app_proto.c 的 ap_json_*）只支持**标量**，
 * 不解析嵌套数组/布尔 —— 这是嵌入式侧的刻意取舍（省 Flash、少出错面）。
 * 因此多会话快照不能下发 `[{...}, {...}]`，而是用 `|` 拼成几个并列字符串：
 *
 *   { n: 2, handles: "01|02", titles: "重构登录|修复乱码",
 *     states: "running|idle", unread: "0|1", sel: "02" }
 *
 * 规则（与 docs/09 §3.3 一致，设备端 app_proto.c 的 ap_flat_next 镜像实现）：
 *   - 分隔符 `|`；字段内的 `|` 转义为 `¦`，`\` 转义为 `╲`；
 *   - 控制字符折叠成空格；
 *   - 标题按 **UTF-8 字节**截断到 SESSION.TITLE_BYTES，且在字符边界截断（不切半个汉字）；
 *   - 单条消息载荷不得超过 MAX_PAYLOAD_BYTES。
 */

import { MAX_PAYLOAD_BYTES, SESSION, SESSION_STATE } from './constants.js'

const SEPARATOR = '|'
const ESCAPED_SEPARATOR = '¦' // U+00A6，UTF-8: C2 A6
const ESCAPED_BACKSLASH = '╲' // U+2572，UTF-8: E2 95 B2
const VALID_HANDLE = /^[0-9a-f]{2}$/
const SESSION_STATES = new Set(Object.values(SESSION_STATE))
const utf8 = new TextEncoder()

/** 字段清洗：控制符→空格，`\` 与 `|` 转义（顺序必须是先反斜杠、后竖线）。 */
export function sanitizeField(value) {
  return String(value ?? '')
    .replace(/[\u0000-\u001f\u007f]/g, ' ')
    .replace(/\\/g, ESCAPED_BACKSLASH)
    .replace(/\|/g, ESCAPED_SEPARATOR)
}

/** 字段还原（与 sanitizeField 互逆；用于主机侧自检/测试）。 */
export function unescapeField(value) {
  return String(value ?? '')
    .split(ESCAPED_SEPARATOR)
    .join('|')
    .split(ESCAPED_BACKSLASH)
    .join('\\')
}

/** 按 UTF-8 字节数截断，保证不切断多字节字符。 */
export function truncateUtf8(text, maxBytes) {
  const limit = Math.max(0, Number(maxBytes) || 0)
  let out = ''
  let used = 0
  for (const ch of String(text ?? '')) {
    const bytes = utf8.encode(ch).length
    if (used + bytes > limit) break
    out += ch
    used += bytes
  }
  return out
}

/** 编码一个标题：清洗 → 截断到 SESSION.TITLE_BYTES。 */
export function encodeTitle(title) {
  return truncateUtf8(sanitizeField(title), SESSION.TITLE_BYTES)
}

/** 载荷的 JSON 字节数（UTF-8）。 */
export function payloadBytes(payload) {
  return utf8.encode(JSON.stringify(payload)).length
}

/**
 * 编码会话快照。
 *
 * @param {Array<{handle:string,title:string,state:string,unread?:boolean}>} sessions
 *   已由主机按 updatedAt 倒序排好；handle 为两位十六进制。
 * @param {string} [sel] 设备当前选中的 handle；空串表示未选。
 * @returns {{n:number,handles:string,titles:string,states:string,unread:string,sel:string}}
 */
export function encodeSessionList(sessions, sel = '') {
  if (!Array.isArray(sessions)) throw new TypeError('sessions 必须是数组')
  if (sessions.length > SESSION.MAX) {
    throw new RangeError(`会话条数 ${sessions.length} 超过上限 ${SESSION.MAX}`)
  }
  const handles = []
  const titles = []
  const states = []
  const unread = []
  for (const session of sessions) {
    const handle = String(session?.handle ?? '')
    if (!VALID_HANDLE.test(handle)) throw new TypeError(`非法 handle：${handle}`)
    const state = String(session?.state ?? '')
    if (!SESSION_STATES.has(state)) throw new TypeError(`非法会话状态：${state}`)
    handles.push(handle)
    titles.push(encodeTitle(session?.title))
    states.push(state)
    unread.push(session?.unread ? '1' : '0')
  }
  if (sel !== '' && !VALID_HANDLE.test(String(sel))) {
    throw new TypeError(`非法 sel：${sel}`)
  }
  const payload = {
    n: sessions.length,
    handles: handles.join(SEPARATOR),
    titles: titles.join(SEPARATOR),
    states: states.join(SEPARATOR),
    unread: unread.join(SEPARATOR),
    sel: sel === '' ? '' : String(sel),
  }
  const size = payloadBytes(payload)
  if (size > MAX_PAYLOAD_BYTES) {
    throw new RangeError(`session.list 载荷 ${size}B 超过上限 ${MAX_PAYLOAD_BYTES}B`)
  }
  return payload
}

/**
 * 解码会话快照（主机侧自检 / 单测用；设备端是 C 的镜像实现）。
 *
 * @param {object|string} payload
 * @returns {{sessions:Array<{handle:string,title:string,state:string,unread:boolean}>, sel:string}}
 */
export function decodeSessionList(payload) {
  const data = typeof payload === 'string' ? JSON.parse(payload) : payload
  const n = Number(data?.n)
  if (!Number.isInteger(n) || n < 0 || n > SESSION.MAX) {
    throw new RangeError(`非法会话条数：${data?.n}`)
  }
  const split = (value) => (n === 0 ? [] : String(value ?? '').split(SEPARATOR))
  const handles = split(data?.handles)
  const titles = split(data?.titles)
  const states = split(data?.states)
  const unread = split(data?.unread)
  for (const [name, list] of Object.entries({ handles, titles, states, unread })) {
    if (list.length !== n) {
      throw new RangeError(`${name} 字段数 ${list.length} 与 n=${n} 不一致`)
    }
  }
  const sessions = handles.map((handle, i) => {
    if (!VALID_HANDLE.test(handle)) throw new TypeError(`非法 handle：${handle}`)
    const state = states[i]
    if (!SESSION_STATES.has(state)) throw new TypeError(`非法会话状态：${state}`)
    return {
      handle,
      title: unescapeField(titles[i]),
      state,
      unread: unread[i] === '1',
    }
  })
  return { sessions, sel: String(data?.sel ?? '') }
}

export const FLAT = Object.freeze({
  SEPARATOR,
  ESCAPED_SEPARATOR,
  ESCAPED_BACKSLASH,
})
