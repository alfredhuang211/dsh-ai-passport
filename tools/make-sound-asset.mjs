#!/usr/bin/env node
// tools/make-sound-asset.mjs —— 把提示音 WAV 转成固件可直接链接的 C 数组
//
// 用途：咳嗽这类"合成不像、采样才像"的音效，用本地采样回放。
//
// ★ 版权：采样文件（QQ 音效等）**不入库**。本工具与生成物都放在被 .gitignore
//   排除的目录里（sound-assets/），只用于你自己的本地构建。
//   若将来要公开发布固件，请换成你自己录制或取得授权的采样，或回落到合成音。
//
// 用法：
//   node tools/make-sound-asset.mjs \
//     --in /path/qq-ke-ke.wav --name alert_cough --out sound-assets/alert_cough.h \
//     --rate 16000 --gain 0.9
//
// 输出头文件包含：
//   #define ALERT_COUGH_SAMPLE_RATE 16000
//   #define ALERT_COUGH_SAMPLE_COUNT <n>
//   static const int16_t ALERT_COUGH_PCM[<n>] = {...};

import { readFileSync, writeFileSync, mkdirSync } from 'node:fs'
import path from 'node:path'

function parseArgs(argv) {
  const out = { rate: 16000, gain: 0.9, trim: true, fadeMs: 3 }
  for (let i = 2; i < argv.length; i += 2) {
    const k = argv[i].replace(/^--/, '')
    const v = argv[i + 1]
    out[k] = ['trim'].includes(k) ? v !== 'false' : Number.isNaN(Number(v)) ? v : Number(v)
  }
  return out
}

/** 读取 16-bit PCM WAV（mono/stereo），返回 {sampleRate, data: Float32Array} */
function readWav(file) {
  const buf = readFileSync(file)
  if (buf.toString('ascii', 0, 4) !== 'RIFF' || buf.toString('ascii', 8, 12) !== 'WAVE') {
    throw new Error('不是 WAV 文件')
  }
  let pos = 12
  let fmt = null
  let data = null
  while (pos + 8 <= buf.length) {
    const id = buf.toString('ascii', pos, pos + 4)
    const size = buf.readUInt32LE(pos + 4)
    const body = pos + 8
    if (id === 'fmt ') {
      fmt = {
        format: buf.readUInt16LE(body),
        channels: buf.readUInt16LE(body + 2),
        sampleRate: buf.readUInt32LE(body + 4),
        bits: buf.readUInt16LE(body + 14),
      }
    } else if (id === 'data') {
      data = buf.subarray(body, body + size)
    }
    pos = body + size + (size % 2)
  }
  if (!fmt || !data) throw new Error('缺少 fmt/data 块')
  if (fmt.format !== 1 || fmt.bits !== 16) throw new Error(`只支持 16-bit PCM，当前 format=${fmt.format} bits=${fmt.bits}`)
  const ch = fmt.channels
  const n = Math.floor(data.length / 2 / ch)
  const x = new Float32Array(n)
  for (let i = 0; i < n; i++) {
    let v = 0
    for (let c = 0; c < ch; c++) v += data.readInt16LE((i * ch + c) * 2) / 32768
    x[i] = v / ch
  }
  return { sampleRate: fmt.sampleRate, data: x }
}

/** 线性重采样到目标采样率 */
function resample(x, from, to) {
  if (from === to) return x
  const n = Math.round((x.length * to) / from)
  const out = new Float32Array(n)
  for (let i = 0; i < n; i++) {
    const p = (i * from) / to
    const i0 = Math.floor(p)
    const i1 = Math.min(x.length - 1, i0 + 1)
    out[i] = x[i0] + (x[i1] - x[i0]) * (p - i0)
  }
  return out
}

/** 去首尾 -45dB 以下静音，留 30ms 余量 */
function trimSilence(x, rate) {
  const thr = 10 ** (-45 / 20)
  let s = 0
  let e = x.length - 1
  while (s < x.length && Math.abs(x[s]) < thr) s++
  while (e > s && Math.abs(x[e]) < thr) e--
  const pad = Math.round(0.03 * rate)
  return x.subarray(Math.max(0, s - pad), Math.min(x.length, e + pad))
}

function normalize(x, gain) {
  let peak = 0
  for (const v of x) peak = Math.max(peak, Math.abs(v))
  if (peak < 1e-9) return x
  const k = gain / peak
  const y = new Float32Array(x.length)
  for (let i = 0; i < x.length; i++) y[i] = x[i] * k
  return y
}

function applyFade(x, rate, ms) {
  const n = Math.round((rate * ms) / 1000)
  const y = x.slice()
  for (let i = 0; i < n && i < y.length; i++) y[i] *= i / n
  for (let i = 0; i < n && i < y.length; i++) y[y.length - 1 - i] *= i / n
  return y
}

const args = parseArgs(process.argv)
if (!args.in || !args.name || !args.out) {
  console.error('用法: node tools/make-sound-asset.mjs --in <wav> --name <c_name> --out <header.h> [--rate 16000] [--gain 0.9] [--trim true]')
  process.exit(1)
}

const { sampleRate, data } = readWav(args.in)
let x = data
if (args.trim) x = trimSilence(x, sampleRate)
x = resample(x, sampleRate, args.rate)
x = applyFade(x, args.rate, args.fadeMs)
x = normalize(x, args.gain)

const count = x.length
const guard = args.name.toUpperCase().replace(/[^A-Z0-9]/g, '_')
const lines = [
  '// 由 tools/make-sound-asset.mjs 生成 —— 不要手改。',
  '// ★ 该文件含第三方音效采样，仅用于本地构建；不要提交到仓库（见 .gitignore）。',
  `#define ${guard}_SAMPLE_RATE ${args.rate}`,
  `#define ${guard}_SAMPLE_COUNT ${count}`,
  `static const int16_t ${guard}_PCM[${count}] = {`,
]
const pcm = []
for (let i = 0; i < count; i++) pcm.push(Math.max(-32768, Math.min(32767, Math.round(x[i] * 32767))))
for (let i = 0; i < pcm.length; i += 16) {
  lines.push('  ' + pcm.slice(i, i + 16).join(', ') + ',')
}
lines.push('};')
lines.push('')

mkdirSync(path.dirname(args.out), { recursive: true })
writeFileSync(args.out, lines.join('\n'))
console.log(`${args.out}  ${count} samples @ ${args.rate}Hz = ${((count * 2) / 1024).toFixed(1)} KB`)
