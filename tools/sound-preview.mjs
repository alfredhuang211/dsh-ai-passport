#!/usr/bin/env node
// tools/sound-preview.mjs —— 设备提示音的离线试听器
//
// 为什么要有它：咳嗽 / BP 机声这类"听感"无法靠读代码判断。固件里烧一次再听，
// 一轮十几分钟；这里用同一套参数在 Mac 上先生成 WAV，调好再改固件常量。
//
// 参考音（仅用于分析参数，不随仓库分发）：
//   - QQ「咳咳」：qq-ke-ke.mp3（早期 QQ 好友上线音）
//   - BP 机提示音："BB机的几种提示音"（bilibili BV1KR4y1K72A）里的 4 连响
// 分析结论：
//   - 咳嗽不是噪声：浊音，f0 从 ~390Hz 下滑到 ~296Hz，能量集中在 ~1.17kHz 共振峰；
//   - BP 机提示音是纯正弦短响：4 声，~140ms/声，间隔 ~110ms，频率 3200Hz。
//
// 固件侧对应实现：main/app_alert.c 的「波形 + 段序列」，咳嗽用共振峰谐波合成、
// 其余用正弦/方波段。改参数时两边同步。
//
// 用法：
//   node tools/sound-preview.mjs
//   afplay sound-preview/approve.wav

import { mkdirSync, writeFileSync } from 'node:fs'
import path from 'node:path'

const SR = 16000
const outDir = (() => {
  const i = process.argv.indexOf('--dir')
  return path.resolve(i >= 0 ? process.argv[i + 1] : 'sound-preview')
})()

const fade = (n) => Math.max(1, Math.round(SR * (n ?? 0.004)))

/** 段：sine / 静音 */
function sine(freq, ms, gain = 1, fadeMs = 0.004) {
  const n = Math.round((SR * ms) / 1000)
  const f = fade(fadeMs)
  const out = new Float32Array(n)
  for (let i = 0; i < n; i++) {
    let v = Math.sin((2 * Math.PI * freq * i) / SR)
    if (i < f) v *= i / f
    else if (i + f > n) v *= (n - i) / f
    out[i] = v * gain
  }
  return out
}
const silence = (ms) => ({ kind: 'sine', freq: 1, ms, gain: 0 })

// ── 咳嗽：声门谐波 + 两个共振峰 + 音高下滑（对齐 QQ 原版特征）────────────
// 参考包络（每 20ms，归一化）：0.0 0.0 0.25 0.771 0.961 0.978 0.893 0.7 0.446
// 0.557 0.688 0.735 0.7 0.594 0.44 0.401 0.377 0.458 0.547 0.586 0.578
// 0.51 0.398 0.272 0.174 0.129 0.133  → 540ms
const COUGH_ENV = [
  0.0, 0.0, 0.25, 0.771, 0.961, 0.978, 0.893, 0.7, 0.446, 0.557, 0.688, 0.735,
  0.7, 0.594, 0.44, 0.401, 0.377, 0.458, 0.547, 0.586, 0.578, 0.51, 0.398,
  0.272, 0.174, 0.129, 0.133,
]

/** 共振峰响应：两个峰（~883 / ~1173Hz）+ 一个弱低峰（~585Hz）。 */
function formantGain(f) {
  const g = (fc, bw) => Math.exp(-((f - fc) ** 2) / (2 * bw ** 2))
  return 1.0 * g(1173, 170) + 0.07 * g(883, 160) + 0.22 * g(585, 140) + 0.004
}

/** 以 f0 为基频、按共振峰分配谐波幅度，合成一个音节的单周期表。 */
function harmonicTable(f0) {
  const amps = []
  for (let k = 1; k <= 8; k++) amps.push(formantGain(k * f0))
  const max = Math.max(...amps)
  return amps.map((a) => a / max)
}

/** 咳嗽（双音节：第一声高、第二声低，带下滑）。 */
function cough({ single = false } = {}) {
  const totalMs = single ? 300 : 540
  const n = Math.round((SR * totalMs) / 1000)
  const out = new Float32Array(n)
  // 音高轮廓：前段 390Hz，235–255ms 滑到 296Hz（单咳不滑）
  let phase = 0
  const sy1 = harmonicTable(390)
  const sy2 = harmonicTable(296)
  const rnd = mulberry32(0x5eed)
  let breathLp = 0
  for (let i = 0; i < n; i++) {
    const tms = (i / SR) * 1000
    let f0 = 390
    if (!single) {
      if (tms >= 255) f0 = 296
      else if (tms > 235) f0 = 390 + ((296 - 390) * (tms - 235)) / 20
    } else {
      f0 = 296
    }
    phase += f0 / SR
    const t = phase % 1
    const amps = single ? sy2 : tms < 245 ? sy1 : sy2
    let v = 0
    for (let k = 1; k <= amps.length; k++) v += amps[k - 1] * Math.sin(2 * Math.PI * k * t)
    // 包络（线性插值 20ms 结点）
    const e =
      tms < totalMs
        ? COUGH_ENV[Math.min(COUGH_ENV.length - 1, Math.floor(tms / 20))] *
          (1 - (tms % 20) / 20) +
          (COUGH_ENV[Math.min(COUGH_ENV.length - 1, Math.floor(tms / 20) + 1)] ?? 0) *
            ((tms % 20) / 20)
        : 0
    // 起音气声（25–75ms）
    const breathEnv = Math.min(1, Math.max(0, (tms - 25) / 25)) * Math.min(1, Math.max(0, (75 - tms) / 25))
    breathLp += 0.35 * (rnd() * 2 - 1 - breathLp)
    // 尾部 5Hz 轻微颤动
    const trem = 1 + 0.04 * Math.sin(2 * Math.PI * 5 * (tms / 1000)) * (tms > 280 ? 1 : 0)
    out[i] = v * e * trem * 0.9 + breathLp * breathEnv * 0.12
  }
  return out
}

function mulberry32(seed) {
  let a = seed >>> 0
  return () => {
    a |= 0
    a = (a + 0x6d2b79f5) | 0
    let t = Math.imul(a ^ (a >>> 15), 1 | a)
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296
  }
}

// ── 音型 ───────────────────────────────────────────────────────────────────
const beeps = (count, onMs, offMs, freq, gain = 0.5) =>
  Array.from({ length: count }, (_, i) => [
    { kind: 'sine', freq, ms: onMs, gain },
    ...(i < count - 1 ? [silence(offMs)] : []),
  ]).flat()

const PATTERNS = {
  // 待审批：QQ 式双咳
  approve: [{ kind: 'cough' }],
  // 追问：单咳（更轻）
  question: [{ kind: 'cough', single: true }],
  // 会话完成：BP 机消息声（原版 BB 机 4 连响：140ms/声，间隔 110ms，3200Hz）
  completed: beeps(4, 140, 110, 3200, 0.5),
  // 备选：4 声长一点（220ms）
  'completed-b': beeps(4, 220, 70, 3200, 0.5),
  // 备选：QQ 滴滴滴式 5 短响
  'completed-c': beeps(5, 40, 90, 3818, 0.45),
  // 会话出错：4 连响 + 下坠低音
  error: [...beeps(4, 140, 110, 3200, 0.5), silence(200), { kind: 'sine', freq: 600, ms: 200, gain: 0.4 }],
  // 会话中断：3 连响
  aborted: beeps(3, 100, 120, 3200, 0.45),
  // 已提交/已选择
  decided: [
    { kind: 'sine', freq: 1200, ms: 70, gain: 0.55 },
    silence(30),
    { kind: 'sine', freq: 1500, ms: 90, gain: 0.55 },
  ],
  pair_ok: [
    { kind: 'sine', freq: 800, ms: 90, gain: 0.5 },
    { kind: 'sine', freq: 1200, ms: 90, gain: 0.5 },
    { kind: 'sine', freq: 1600, ms: 120, gain: 0.5 },
  ],
  low_battery: [
    { kind: 'sine', freq: 500, ms: 120, gain: 0.45 },
    silence(100),
    { kind: 'sine', freq: 500, ms: 120, gain: 0.45 },
  ],
}

/** 段序列 → Float32Array */
function render(segs) {
  const parts = []
  for (const s of segs) {
    if (s.kind === 'cough') parts.push(cough({ single: s.single }))
    else if (s.freq === 1 && s.gain === 0) parts.push(new Float32Array(Math.round((SR * s.ms) / 1000)))
    else parts.push(sine(s.freq, s.ms, s.gain ?? 1))
  }
  const total = parts.reduce((a, p) => a + p.length, 0)
  const out = new Float32Array(total)
  let at = 0
  for (const p of parts) {
    out.set(p, at)
    at += p.length
  }
  return out
}

function toWav(samples) {
  const n = samples.length
  const buf = Buffer.alloc(44 + n * 2)
  buf.write('RIFF', 0)
  buf.writeUInt32LE(36 + n * 2, 4)
  buf.write('WAVE', 8)
  buf.write('fmt ', 12)
  buf.writeUInt32LE(16, 16)
  buf.writeUInt16LE(1, 20)
  buf.writeUInt16LE(1, 22)
  buf.writeUInt32LE(SR, 24)
  buf.writeUInt32LE(SR * 2, 28)
  buf.writeUInt16LE(2, 32)
  buf.writeUInt16LE(16, 34)
  buf.write('data', 36)
  buf.writeUInt32LE(n * 2, 40)
  for (let i = 0; i < n; i++) {
    const v = Math.max(-1, Math.min(1, samples[i] * 0.85))
    buf.writeInt16LE(Math.round(v * 32767), 44 + i * 2)
  }
  return buf
}

mkdirSync(outDir, { recursive: true })
for (const [name, segs] of Object.entries(PATTERNS)) {
  const samples = render(segs)
  writeFileSync(path.join(outDir, `${name}.wav`), toWav(samples))
  console.log(`${name}.wav  ${(samples.length / SR).toFixed(2)}s`)
}
console.log(`\n试听：afplay ${path.join(outDir, 'approve.wav')}`)
