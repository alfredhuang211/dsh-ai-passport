/**
 * 多会话域（协议 v2）：把 DSH 的会话列表/状态/完成事件聚合后推给设备。
 *
 * ══════════════════════════════════════════════════════════════════════════
 *  与旧 StateDomain（v1）的关系
 * ══════════════════════════════════════════════════════════════════════════
 *  v1 只聚合出一个"全局四态"（idle / running / waiting_approval / completed）。
 *  v2 设备要显示**多个会话**并能选择，因此这里维护会话表，并同时：
 *    - 对 v2 设备：下发 session.list / session.detail / session.done；
 *    - 对 v1 旧固件：继续用旧的 task.state 聚合（D9 兼容路径）；
 *    - 对面板/设置页：snapshot() 里同时给出会话数组与旧的聚合字段。
 *
 *  ★ 两套输出必须来自**同一份会话表**，否则设备与桌面会显示不一致。
 *
 * ══════════════════════════════════════════════════════════════════════════
 *  数据来源（DSH 宿主契约，调研结论见 docs/09 §4.1）
 * ══════════════════════════════════════════════════════════════════════════
 *   - api-session/added(summary) / removed(id) / status(id, running)
 *     / activity(id, updatedAt) / error(id, message)
 *   - session/event(session, event)：event.type = turn/start | turn/end |
 *     session/title | tool/call | assistant/message …
 *   - sessionController.list({}) 冷启动补全（返回 {items: SessionSummary[]}）
 *
 *  ★ 标题在 summary.projections.values.title（不是 summary.title）；
 *    turn/end 的结束原因在 event.data.reason.kind。
 */

import { MSG, SESSION, SESSION_STATE, TASK_STATE } from '../protocol/constants.js'
import { encodeSessionList, encodeTitle } from '../protocol/flat.js'

/** 「已完成」在旧四态快照里停留的时长（v1 兼容输出用）。 */
const COMPLETED_LINGER_MS = 8000
/** 变更合并窗口：避免每个工具事件都推一次列表（设备端只认快照）。 */
const PUSH_COALESCE_MS = 250
/** 会话页详情刷新节流。 */
const DETAIL_THROTTLE_MS = 1000
/** 子代理会话不上设备列表。 */
const SUBAGENT_ORIGIN = 'subagent'
const ERROR_REASONS = new Set(['error', 'blocked'])
const ABORTED_REASONS = new Set(['aborted', 'interrupted'])

const now = () => Date.now()

function handleToHex(n) {
  return (n & 0xff).toString(16).padStart(2, '0')
}

function titleOf(summary) {
  return String(summary?.projections?.values?.title ?? summary?.title ?? '').trim()
}

/** 从 assistant/message 事件里取纯文本摘要（设备只显示一行）。 */
function assistantText(message) {
  const content = message?.message?.content
  if (!Array.isArray(content)) return ''
  return content
    .filter((part) => part?.type === 'text')
    .map((part) => String(part.text ?? ''))
    .join(' ')
    .trim()
}

export class SessionDomain {
  constructor(shared = {}) {
    this.link = shared.link
    this.logger = shared.logger ?? (() => {})
    this.domainState = shared.domainState ?? null
    this.config = shared.config ?? {}
    this.ctx = null
    this.runtime = null

    this.unsubscribes = []
    this.disposers = []

    /** @type {Map<string, object>} sessionId → 会话记录 */
    this.sessions = new Map()
    /** @type {Map<string, string>} sessionId → handle */
    this.handleBySession = new Map()
    /** @type {Map<string, string>} handle → sessionId */
    this.sessionByHandle = new Map()
    this.nextHandle = 1
    /** 设备当前选中的 handle（空串 = 未选）。 */
    this.selectedHandle = ''
    /** 设备正在录音时抑制列表推送（避免"说话时列表跳"）。 */
    this.deviceRecording = false

    this.pushTimer = null
    this.detailTimer = null
    this.completedTimer = null

    this.lastSentKey = null
    this.lastLegacyKey = null
    this.lastCompletedTitle = ''
    /** 旧四态兼容字段；集成测试会直接改它来跳过 8 秒等待。 */
    this.completedUntil = 0
    this.approvalTitle = null

    this.initialLoad = null
    this.disposed = false
  }

  // ── 生命周期 ──────────────────────────────────────────────────────────────

  attach(runtime, ctx) {
    this.runtime = runtime
    if (ctx && ctx !== this.ctx) {
      this.ctx = ctx
      this.#subscribe()
      this.#registerHandlers()
      void this.#loadInitial()
    }
    this.push({ force: true, immediate: true })
  }

  detach() {
    for (const unsubscribe of this.unsubscribes) {
      try {
        if (typeof unsubscribe === 'function') unsubscribe()
      } catch {
        // 宿主已拆除
      }
    }
    this.unsubscribes.length = 0
    for (const dispose of this.disposers) {
      try {
        if (typeof dispose === 'function') dispose()
      } catch {
        // 同上
      }
    }
    this.disposers.length = 0
    this.#clearTimers()
    this.deviceRecording = false
  }

  dispose() {
    if (this.disposed) return
    this.disposed = true
    this.detach()
  }

  #clearTimers() {
    if (this.pushTimer) clearTimeout(this.pushTimer)
    if (this.detailTimer) clearTimeout(this.detailTimer)
    if (this.completedTimer) clearTimeout(this.completedTimer)
    this.pushTimer = null
    this.detailTimer = null
    this.completedTimer = null
  }

  // ── 宿主订阅 ──────────────────────────────────────────────────────────────

  #subscribe() {
    const ctx = this.ctx
    if (!ctx?.on) {
      this.logger('warn', '[sessions] 宿主上下文不可用，会话状态将不会自动更新')
      return
    }
    const bind = (name, handler) => {
      const off = ctx.on(name, handler)
      this.unsubscribes.push(typeof off === 'function' ? off : () => ctx.off?.(name, handler))
    }

    bind('api-session/added', (summary) => this.#upsertSummary(summary))
    bind('api-session/removed', (sessionId) => this.#removeSession(sessionId))
    bind('api-session/status', (sessionId, running) => this.#setRunning(sessionId, running))
    bind('api-session/activity', (sessionId, updatedAt) => this.#touch(sessionId, updatedAt))
    bind('api-session/error', (sessionId, message) => this.#setError(sessionId, message))
    bind('session/event', (session, event) => {
      try {
        this.#onSessionEvent(session, event)
      } catch (error) {
        this.logger('debug', `[sessions] 处理会话事件失败：${error?.message ?? error}`)
      }
    })
  }

  #registerHandlers() {
    if (!this.link?.onMessage) return
    this.disposers.push(
      this.link.onMessage(MSG.SESSION_SELECT, (message) => this.#onDeviceSelect(message)),
      this.link.onMessage(MSG.SESSION_LIST_REQ, () => this.push({ force: true, immediate: true })),
      this.link.onMessage(MSG.SESSION_DETAIL_REQ, (message) => this.#onDetailReq(message)),
    )
  }

  async #loadInitial() {
    const controller = typeof this.ctx?.get === 'function' ? this.ctx.get('sessionController') : null
    if (!controller?.list) return
    try {
      const result = await controller.list({})
      const items = Array.isArray(result) ? result : (result?.items ?? [])
      for (const item of items) this.#upsertSummary(item, { silent: true })
      this.push({ force: true })
    } catch (error) {
      this.logger('debug', `[sessions] 初始会话列表读取失败：${error?.message ?? error}`)
    }
  }

  /** 事件里没带标题时，回查一次 sessionController.list()（异步，不阻塞事件）。 */
  async #lookupTitle(sessionId) {
    const controller = typeof this.ctx?.get === 'function' ? this.ctx.get('sessionController') : null
    if (!controller?.list) return ''
    try {
      const result = await controller.list({})
      const items = Array.isArray(result) ? result : (result?.items ?? [])
      const item = items.find((entry) => (entry?.sessionId ?? entry?.id) === sessionId)
      return titleOf(item)
    } catch {
      return ''
    }
  }

  // ── 会话表维护 ────────────────────────────────────────────────────────────

  #recordOf(sessionId, create = true) {
    const id = String(sessionId ?? '')
    if (!id) return null
    let record = this.sessions.get(id)
    if (!record && create) {
      record = {
        sessionId: id,
        title: '',
        running: false,
        waiting: false,
        lastEnd: null,
        unread: false,
        error: null,
        updatedAt: 0,
        startedAt: 0,
        endedAt: 0,
        step: null,
        stepTotal: null,
        tool: '',
        summary: '',
        origin: undefined,
        blank: false,
      }
      this.sessions.set(id, record)
    }
    return record ?? null
  }

  #upsertSummary(summary, options = {}) {
    const id = summary?.sessionId ?? summary?.id
    if (!id) return
    const record = this.#recordOf(id)
    if (!record) return
    const title = titleOf(summary)
    if (title) record.title = title
    if (typeof summary?.running === 'boolean') {
      record.running = summary.running
      if (summary.running) record.lastEnd = null
    }
    if (typeof summary?.blank === 'boolean') record.blank = summary.blank
    if (summary?.origin !== undefined) record.origin = summary.origin
    if (summary?.projections?.values?.step != null) record.step = Number(summary.projections.values.step)
    if (summary?.projections?.values?.tool != null) record.tool = String(summary.projections.values.tool)
    const at = Number(summary?.updatedAt)
    if (Number.isFinite(at) && at > 0) record.updatedAt = Math.max(record.updatedAt, at)
    else if (!record.updatedAt) record.updatedAt = now()
    if (!options.silent) this.push()
  }

  #removeSession(sessionId) {
    const id = String(sessionId ?? '')
    const record = this.sessions.get(id)
    if (!record) return
    this.#freeHandle(record)
    this.sessions.delete(id)
    if (record.unread && record.handle) {
      // 会话消失后未读没有承载对象，直接丢弃。
    }
    this.push()
  }

  #setRunning(sessionId, running) {
    const record = this.#recordOf(sessionId)
    if (!record) return
    record.running = Boolean(running)
    if (record.running) record.lastEnd = null
    record.updatedAt = now()
    this.push()
  }

  #touch(sessionId, updatedAt) {
    const record = this.#recordOf(sessionId)
    if (!record) return
    const at = Number(updatedAt)
    if (Number.isFinite(at)) record.updatedAt = Math.max(record.updatedAt, at)
    else record.updatedAt = now()
    this.push()
  }

  #setError(sessionId, message) {
    const record = this.#recordOf(sessionId)
    if (!record) return
    record.error = String(message ?? '')
    record.lastEnd = 'error'
    record.updatedAt = now()
    this.push()
  }

  #onSessionEvent(session, event) {
    const id = session?.id ?? session?.sessionId ?? event?.sessionId
    const type = event?.type
    if (!id || !type) return
    const record = this.#recordOf(id)
    if (!record) return
    const data = event.data ?? {}
    // 事件里的 session 只有 id；标题可能只在 projections 或需要回查 list()。
    const inlineTitle = String(session?.projections?.values?.title ?? session?.title ?? '').trim()
    if (inlineTitle) record.title = inlineTitle

    switch (type) {
      case 'turn/start':
        record.running = true
        record.lastEnd = null
        record.startedAt = Number(event.time) || now()
        record.endedAt = 0
        record.updatedAt = now()
        this.push()
        break

      case 'turn/end': {
        record.running = false
        record.lastEnd = String(data?.reason?.kind ?? 'completed')
        record.endedAt = Number(event.time) || now()
        record.updatedAt = now()
        record.unread = this.#deviceSelectedSessionId() !== id
        this.#markCompleted(record)
        if (!record.title) {
          void this.#lookupTitle(id).then((title) => {
            if (!title) return
            record.title = title
            if (now() < this.completedUntil) this.lastCompletedTitle = title
            this.push({ force: true })
            this.#sendDone(record)
          })
        }
        // 先推列表、再发 done：设备收到 done 时列表里已经有该 handle。
        this.push({ force: true, immediate: true })
        this.#sendDone(record)
        break
      }

      case 'session/title':
        if (data?.title) record.title = String(data.title)
        record.updatedAt = now()
        this.push()
        break

      case 'tool/call':
        record.tool = String(data?.name ?? record.tool ?? '')
        if (data?.step != null) record.step = Number(data.step)
        this.#scheduleDetail(record)
        break

      case 'assistant/message': {
        const text = assistantText(data)
        if (text) record.summary = text
        this.#scheduleDetail(record)
        break
      }

      default:
        break
    }
  }

  #markCompleted(record) {
    this.completedUntil = now() + COMPLETED_LINGER_MS
    this.lastCompletedTitle = record.title || ''
    if (this.completedTimer) clearTimeout(this.completedTimer)
    this.completedTimer = setTimeout(() => {
      this.completedTimer = null
      this.push()
    }, COMPLETED_LINGER_MS + 50)
    this.completedTimer.unref?.()
  }

  // ── handle 分配 ───────────────────────────────────────────────────────────

  #ensureHandle(record) {
    const existing = this.handleBySession.get(record.sessionId)
    if (existing) return existing
    let attempts = 0
    while (attempts < 0x100) {
      const handle = handleToHex(this.nextHandle)
      this.nextHandle = (this.nextHandle + 1) & 0xff
      if (this.nextHandle === 0) this.nextHandle = 1
      if (this.sessionByHandle.has(handle)) {
        attempts++
        continue
      }
      this.handleBySession.set(record.sessionId, handle)
      this.sessionByHandle.set(handle, record.sessionId)
      record.handle = handle
      return handle
    }
    return ''
  }

  #freeHandle(record) {
    if (!record?.handle) return
    if (this.handleBySession.get(record.sessionId) === record.handle) {
      this.handleBySession.delete(record.sessionId)
    }
    this.sessionByHandle.delete(record.handle)
    record.handle = ''
    if (this.selectedHandle === record.handle) this.selectedHandle = ''
  }

  #resetHandles() {
    this.handleBySession.clear()
    this.sessionByHandle.clear()
    this.nextHandle = 1
    this.selectedHandle = ''
    for (const record of this.sessions.values()) record.handle = ''
  }

  #deviceSelectedSessionId() {
    if (!this.selectedHandle) return ''
    return this.sessionByHandle.get(this.selectedHandle) ?? ''
  }

  // ── 状态计算 ──────────────────────────────────────────────────────────────

  #stateOf(record) {
    if (record.waiting) return SESSION_STATE.WAITING
    if (record.running) return SESSION_STATE.RUNNING
    if (record.lastEnd && ERROR_REASONS.has(record.lastEnd)) return SESSION_STATE.ERROR
    if (record.lastEnd && ABORTED_REASONS.has(record.lastEnd)) return SESSION_STATE.ABORTED
    return SESSION_STATE.IDLE
  }

  /** 旧四态聚合（v1 兼容输出与面板快照共用）。 */
  #legacyState() {
    if (this.approvalTitle) {
      return { state: TASK_STATE.WAITING_APPROVAL, title: this.approvalTitle }
    }
    let running = 0
    for (const record of this.sessions.values()) if (record.running) running++
    if (running > 0) return { state: TASK_STATE.RUNNING, title: `${running} 个任务执行中` }
    if (now() < this.completedUntil) {
      return { state: TASK_STATE.COMPLETED, title: this.lastCompletedTitle || '任务刚刚结束' }
    }
    return { state: TASK_STATE.IDLE, title: '' }
  }

  /** 上设备的会话：排除子代理与空白（运行中的空白保留），按 updatedAt 倒序。 */
  visibleSessions() {
    const list = []
    for (const record of this.sessions.values()) {
      if (record.origin === SUBAGENT_ORIGIN) continue
      if (record.blank && !record.running) continue
      this.#ensureHandle(record)
      list.push(record)
    }
    list.sort((a, b) => {
      if (b.updatedAt !== a.updatedAt) return b.updatedAt - a.updatedAt
      return String(a.handle).localeCompare(String(b.handle))
    })
    const visible = list.slice(0, SESSION.MAX)
    const visibleHandles = new Set(visible.map((record) => record.handle))
    if (this.selectedHandle && !visibleHandles.has(this.selectedHandle)) {
      this.selectedHandle = ''
    }
    return visible
  }

  // ── 推送 ──────────────────────────────────────────────────────────────────

  /**
   * 请求一次推送。默认按 PUSH_COALESCE_MS 合并；
   * `immediate` 用于握手/选择/完成这类必须立刻生效的时机。
   */
  push(options = {}) {
    if (this.disposed) return
    if (this.deviceRecording) return
    if (options.immediate) {
      if (this.pushTimer) {
        clearTimeout(this.pushTimer)
        this.pushTimer = null
      }
      this.#flush(options)
      return
    }
    if (this.pushTimer) return
    this.pushTimer = setTimeout(() => {
      this.pushTimer = null
      this.#flush(options)
    }, PUSH_COALESCE_MS)
    this.pushTimer.unref?.()
  }

  /** 立即推送（测试与握手路径用）。 */
  flush() {
    if (this.pushTimer) {
      clearTimeout(this.pushTimer)
      this.pushTimer = null
    }
    this.#flush({ force: true })
  }

  #linkSnapshot() {
    return typeof this.link?.snapshot === 'function' ? this.link.snapshot() : {}
  }

  #flush(options = {}) {
    if (this.deviceRecording) return
    const snapshot = this.#linkSnapshot()
    if (snapshot.state !== 'connected' || !snapshot.handshakeComplete) return
    const deviceVersion = Number(snapshot.deviceInfo?.protocolVersion) || 0
    if (deviceVersion >= 2) this.#flushSessions(options)
    else this.#flushLegacy(options)
    this.#syncDomainState()
  }

  #flushSessions(options) {
    const visible = this.visibleSessions()
    let payload
    try {
      payload = encodeSessionList(
        visible.map((record) => ({
          handle: record.handle,
          title: record.title,
          state: this.#stateOf(record),
          unread: Boolean(record.unread),
        })),
        this.selectedHandle,
      )
    } catch (error) {
      this.logger('warn', `[sessions] 会话快照编码失败：${error?.message ?? error}`)
      return
    }
    const key = JSON.stringify(payload)
    if (!options.force && key === this.lastSentKey) return
    this.lastSentKey = key
    Promise.resolve(this.link.send(MSG.SESSION_LIST, payload)).catch((error) =>
      this.logger('debug', `[sessions] session.list 下发失败：${error?.message ?? error}`),
    )
  }

  #flushLegacy(options) {
    const legacy = this.#legacyState()
    const key = `${legacy.state}:${legacy.title}`
    if (!options.force && key === this.lastLegacyKey) return
    this.lastLegacyKey = key
    Promise.resolve(this.link.send(MSG.TASK_STATE, legacy)).catch((error) =>
      this.logger('debug', `[sessions] task.state 下发失败：${error?.message ?? error}`),
    )
  }

  #syncDomainState() {
    if (!this.domainState) return
    const legacy = this.#legacyState()
    this.domainState.taskState = { ...legacy, at: now() }
    this.domainState.sessions = this.snapshot().sessions
    this.domainState.selectedHandle = this.selectedHandle
    this.domainState.deviceSelectedSessionId = this.#deviceSelectedSessionId()
  }

  // ── 设备请求 ──────────────────────────────────────────────────────────────

  #onDeviceSelect(message) {
    const handle = String(message?.h ?? '')
    const sessionId = this.sessionByHandle.get(handle)
    if (!sessionId) {
      this.#toast('会话已关闭')
      this.push({ force: true, immediate: true })
      return
    }
    this.selectedHandle = handle
    const record = this.sessions.get(sessionId)
    if (record) record.unread = false
    this.push({ force: true, immediate: true })
    this.#sendDetail(record)
  }

  #onDetailReq(message) {
    const handle = String(message?.h ?? '')
    const sessionId = this.sessionByHandle.get(handle)
    const record = sessionId ? this.sessions.get(sessionId) : null
    if (!record) {
      this.#toast('会话已关闭')
      return
    }
    this.#sendDetail(record)
  }

  #scheduleDetail(record) {
    if (!record || this.selectedHandle === '') return
    if (this.sessionByHandle.get(this.selectedHandle) !== record.sessionId) return
    if (this.detailTimer) return
    this.detailTimer = setTimeout(() => {
      this.detailTimer = null
      this.#sendDetail(record)
    }, DETAIL_THROTTLE_MS)
    this.detailTimer.unref?.()
  }

  #sendDetail(record) {
    if (!record) return
    const snapshot = this.#linkSnapshot()
    if (snapshot.state !== 'connected' || !snapshot.handshakeComplete) return
    if ((Number(snapshot.deviceInfo?.protocolVersion) || 0) < 2) return
    const handle = this.#ensureHandle(record)
    const payload = {
      h: handle,
      title: encodeTitle(record.title),
      state: this.#stateOf(record),
    }
    if (record.step != null) payload.step = Number(record.step)
    if (record.stepTotal != null) payload.stepTotal = Number(record.stepTotal)
    if (record.tool) payload.tool = String(record.tool)
    if (record.startedAt && record.running) payload.elapsedMs = Math.max(0, now() - record.startedAt)
    else if (record.startedAt && record.endedAt) payload.elapsedMs = Math.max(0, record.endedAt - record.startedAt)
    if (record.summary) payload.summary = encodeTitle(record.summary)
    Promise.resolve(this.link.send(MSG.SESSION_DETAIL, payload)).catch((error) =>
      this.logger('debug', `[sessions] session.detail 下发失败：${error?.message ?? error}`),
    )
  }

  #sendDone(record) {
    const snapshot = this.#linkSnapshot()
    if (snapshot.state !== 'connected' || !snapshot.handshakeComplete) return
    if ((Number(snapshot.deviceInfo?.protocolVersion) || 0) < 2) return
    const handle = this.#ensureHandle(record)
    const payload = {
      h: handle,
      title: encodeTitle(record.title),
      result: this.#doneResult(record),
      durationMs: record.startedAt ? Math.max(0, (record.endedAt || now()) - record.startedAt) : 0,
    }
    Promise.resolve(this.link.send(MSG.SESSION_DONE, payload)).catch((error) =>
      this.logger('debug', `[sessions] session.done 下发失败：${error?.message ?? error}`),
    )
  }

  #doneResult(record) {
    if (!record.lastEnd) return 'completed'
    if (ERROR_REASONS.has(record.lastEnd)) return 'error'
    if (ABORTED_REASONS.has(record.lastEnd)) return 'aborted'
    return 'completed'
  }

  #toast(text) {
    Promise.resolve(this.link.send(MSG.TOAST, { text })).catch(() => {})
  }

  // ── 对外接口（兼容旧 StateDomain 的调用点）─────────────────────────────────

  /** 审批挂起状态（S3 会扩展为按会话；当前保持 v1 签名）。 */
  setApproval(title) {
    const next = title ? String(title).slice(0, 32) : null
    if (this.approvalTitle === next) return
    this.approvalTitle = next
    this.push()
  }

  /** 设备录音开始/结束：录音期间抑制列表推送。 */
  setDeviceRecording(recording) {
    const next = Boolean(recording)
    if (this.deviceRecording === next) return
    this.deviceRecording = next
    if (!next) this.push()
  }

  /** 链路就绪：重新分配 handle、清掉去重记忆并立即补推。 */
  onLinkReady() {
    this.#resetHandles()
    this.lastSentKey = null
    this.lastLegacyKey = null
    this.push({ force: true, immediate: true })
  }

  /** 真正断链：清录音标志与选中（设备侧同样会清）。 */
  onLinkReset() {
    this.deviceRecording = false
    this.selectedHandle = ''
    this.#resetHandles()
    this.lastSentKey = null
    this.lastLegacyKey = null
  }

  /** 面板/测试用快照：旧四态字段 + 新会话数组。 */
  snapshot() {
    const legacy = this.#legacyState()
    const sessions = this.visibleSessions().map((record) => ({
      sessionId: record.sessionId,
      handle: record.handle,
      title: record.title,
      state: this.#stateOf(record),
      unread: Boolean(record.unread),
      updatedAt: record.updatedAt,
      running: Boolean(record.running),
    }))
    return {
      state: legacy.state,
      title: legacy.title,
      sessions,
      selectedHandle: this.selectedHandle,
      deviceSelectedSessionId: this.#deviceSelectedSessionId(),
      runningCount: sessions.filter((session) => session.running).length,
      waitingCount: sessions.filter((session) => session.state === SESSION_STATE.WAITING).length,
    }
  }
}
