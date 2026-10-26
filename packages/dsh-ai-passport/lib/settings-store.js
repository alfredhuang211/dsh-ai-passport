/**
 * 运行时可改设置（桌面设置页 → 设备/域即时生效）。
 *
 * 为什么不用 DSH 官方设置服务：
 *  - `ctx.settings` 只收录带 `.volatile()` 的字段，而本插件的 Config schema 没有；
 *  - 即使加了 volatile，`settings.update()` 走 Loader 热应用 → 插件重新 apply →
 *    BLE 链路重建。本项目最脆的一环就是链路，不能因为"改个提示音开关"断一次。
 * 因此设置落在插件自己的 JSON 文件里，域在每次读取时取最新值。
 *
 * 只读项（transport/autoConnect/providerId…）故意不在这里：它们影响启动路径，
 * 改动必须重启，设置页只做展示。
 */

import fs from 'node:fs'
import path from 'node:path'

import { SESSION } from './protocol/constants.js'

/** 可写项与默认值。maxSessions 设备端上限是 SESSION.MAX（8）。 */
export const SETTINGS_DEFAULTS = Object.freeze({
  maskTitles: false,
  soundEnabled: true,
  backlightPulse: true,
  detailSummary: true,
  detailElapsed: true,
  voiceAutoSend: false,
  maxSessions: SESSION.MAX,
})

const BOOLEAN_KEYS = ['maskTitles', 'soundEnabled', 'backlightPulse', 'detailSummary', 'detailElapsed', 'voiceAutoSend']

function clampInt(value, min, max, fallback) {
  const n = Number(value)
  if (!Number.isFinite(n)) return fallback
  return Math.min(max, Math.max(min, Math.trunc(n)))
}

export class SettingsStore {
  /**
   * @param {{filePath?: string, logger?: Function, defaults?: object}} options
   */
  constructor({ filePath = '', logger = () => {}, defaults = {} } = {}) {
    this.filePath = filePath
    this.logger = logger
    this.values = { ...SETTINGS_DEFAULTS }
    for (const key of Object.keys(SETTINGS_DEFAULTS)) {
      if (defaults[key] !== undefined) this.values[key] = defaults[key]
    }
  }

  /** 从磁盘读取（文件不存在/损坏都回落到默认值，不阻塞插件启动）。 */
  load() {
    if (!this.filePath) return this.get()
    try {
      const raw = fs.readFileSync(this.filePath, 'utf8')
      const parsed = JSON.parse(raw)
      const result = this.#normalize(parsed)
      this.values = { ...this.values, ...result.values }
      for (const warning of result.warnings) this.logger('debug', `[settings] ${warning}`)
    } catch (error) {
      if (error?.code !== 'ENOENT') {
        this.logger('warn', `[settings] 读取设置失败，使用默认值：${error?.message ?? error}`)
      }
    }
    return this.get()
  }

  /** 当前设置（副本，调用方不能改内部对象）。 */
  get() {
    return { ...this.values }
  }

  /**
   * 合并写入并持久化。
   *
   * @param {object} patch 只传要改的字段；未知字段忽略。
   * @returns {{settings: object, changed: boolean, warnings: string[]}}
   */
  set(patch) {
    const result = this.#normalize(patch ?? {})
    const next = { ...this.values, ...result.values }
    const changed = Object.keys(next).some((key) => next[key] !== this.values[key])
    this.values = next
    if (changed) this.#save()
    return { settings: this.get(), changed, warnings: result.warnings }
  }

  #normalize(input) {
    const values = {}
    const warnings = []
    if (!input || typeof input !== 'object') {
      return { values, warnings: ['设置补丁不是对象，已忽略'] }
    }
    for (const key of BOOLEAN_KEYS) {
      if (input[key] === undefined) continue
      if (typeof input[key] === 'boolean') values[key] = input[key]
      else warnings.push(`${key} 必须是布尔值，已忽略`)
    }
    if (input.maxSessions !== undefined) {
      const clamped = clampInt(input.maxSessions, 1, SESSION.MAX, NaN)
      if (Number.isFinite(clamped)) values.maxSessions = clamped
      else warnings.push(`maxSessions 非法（${input.maxSessions}），已忽略`)
    }
    return { values, warnings }
  }

  #save() {
    if (!this.filePath) return
    try {
      fs.mkdirSync(path.dirname(this.filePath), { recursive: true })
      const tmp = `${this.filePath}.tmp`
      fs.writeFileSync(tmp, `${JSON.stringify(this.values, null, 2)}\n`, 'utf8')
      fs.renameSync(tmp, this.filePath)
    } catch (error) {
      this.logger('warn', `[settings] 保存设置失败：${error?.message ?? error}`)
    }
  }
}
