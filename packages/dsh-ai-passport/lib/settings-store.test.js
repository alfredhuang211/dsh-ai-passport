/**
 * 运行时设置存储测试：默认值、持久化往返、钳制、损坏文件容错。
 * 这些行为决定"改完设置重启后还在不在"，属于用户能直接感知的可靠性。
 */

import { test } from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'

import { SettingsStore, SETTINGS_DEFAULTS } from './settings-store.js'

function tmpFile() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ap-settings-'))
  return path.join(dir, 'settings.json')
}

test('默认值：与 SETTINGS_DEFAULTS 一致', () => {
  const store = new SettingsStore()
  assert.deepEqual(store.get(), { ...SETTINGS_DEFAULTS })
})

test('设置补丁只改传入字段，其它保持', () => {
  const store = new SettingsStore()
  const result = store.set({ maskTitles: true })
  assert.equal(result.changed, true)
  assert.equal(store.get().maskTitles, true)
  assert.equal(store.get().soundEnabled, SETTINGS_DEFAULTS.soundEnabled)
})

test('持久化往返：写入后新实例能读回', () => {
  const filePath = tmpFile()
  const store = new SettingsStore({ filePath })
  store.set({ maskTitles: true, maxSessions: 3, detailSummary: false })
  const reloaded = new SettingsStore({ filePath }).load()
  assert.equal(reloaded.maskTitles, true)
  assert.equal(reloaded.maxSessions, 3)
  assert.equal(reloaded.detailSummary, false)
})

test('非法值与越界被钳制/忽略，不抛错', () => {
  const store = new SettingsStore()
  const result = store.set({ maxSessions: 99, soundEnabled: 'yes', unknown: 1 })
  assert.equal(store.get().maxSessions, 8, '上限钳到 SESSION.MAX')
  assert.equal(store.get().soundEnabled, SETTINGS_DEFAULTS.soundEnabled, '非布尔忽略')
  assert.ok(result.warnings.length >= 1, '非法布尔值应产生告警')
  assert.equal('unknown' in store.get(), false, '未知字段不应落盘')
  store.set({ maxSessions: 0 })
  assert.equal(store.get().maxSessions, 1, '下限钳到 1')
})

test('文件损坏时回落默认值，不让插件启动失败', () => {
  const filePath = tmpFile()
  fs.writeFileSync(filePath, '{ this is not json', 'utf8')
  const warnings = []
  const store = new SettingsStore({ filePath, logger: (level, message) => warnings.push(message) })
  store.load()
  assert.deepEqual(store.get(), { ...SETTINGS_DEFAULTS })
  assert.ok(warnings.some((line) => line.includes('读取设置失败')))
})

test('未知字段不写进结果（避免 UI 误塞进来污染文件）', () => {
  const store = new SettingsStore()
  store.set({ maskTitles: true, evil: 'x' })
  assert.equal('evil' in store.get(), false)
})
