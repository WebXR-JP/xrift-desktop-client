// 同期 fetch（ディスクキャッシュ付き）
//
// module.registerHooks() のフックは同期的に結果を返す必要があるため、ネットワーク取得だけを
// ワーカースレッドに任せ、メインスレッドは Atomics.wait でその完了を待つ。
// XRift の配信物（assets.xrift.net/.../<版ハッシュ>/...）は版ごとに URL が変わるので、
// 一度取れたものは URL をキーにずっとキャッシュしてよい。

import { createHash } from 'node:crypto'
import { existsSync, mkdirSync, readFileSync } from 'node:fs'
import { join } from 'node:path'
import { Worker } from 'node:worker_threads'
import { cacheDir } from './paths.js'

const WORKER_SOURCE = `
const { parentPort } = require('node:worker_threads')
const { writeFileSync, renameSync } = require('node:fs')
parentPort.on('message', async ({ url, file, control, errorBuf }) => {
  const ctl = new Int32Array(control)
  const err = new Uint8Array(errorBuf)
  try {
    const res = await fetch(url)
    if (!res.ok) throw new Error('HTTP ' + res.status + ' ' + res.statusText)
    const data = Buffer.from(await res.arrayBuffer())
    const tmp = file + '.' + process.pid + '.tmp'
    writeFileSync(tmp, data)
    renameSync(tmp, file)
    Atomics.store(ctl, 1, 0)
  } catch (e) {
    const msg = Buffer.from(String(e && e.message || e)).subarray(0, err.length - 1)
    err.fill(0); err.set(msg)
    Atomics.store(ctl, 1, 1)
  }
  Atomics.store(ctl, 0, 1)
  Atomics.notify(ctl, 0)
})
`

let worker = null
const control = new SharedArrayBuffer(8)
const errorBuf = new SharedArrayBuffer(4096)

function getWorker() {
  if (!worker) {
    worker = new Worker(WORKER_SOURCE, { eval: true })
    worker.unref()
  }
  return worker
}

export function cachePathFor(url) {
  const hash = createHash('sha1').update(url).digest('hex')
  const ext = (new URL(url).pathname.match(/\.[a-z0-9]+$/i) || [''])[0]
  return join(cacheDir('http'), hash + ext)
}

/** URL を同期的に取得してローカルのキャッシュファイルパスを返す */
export function syncFetchToFile(url, { noCache = false } = {}) {
  const file = cachePathFor(url)
  if (!noCache && existsSync(file)) return file
  mkdirSync(cacheDir('http'), { recursive: true })
  const ctl = new Int32Array(control)
  Atomics.store(ctl, 0, 0)
  getWorker().postMessage({ url, file, control, errorBuf })
  // 最大 120 秒待つ
  const r = Atomics.wait(ctl, 0, 0, 120_000)
  if (r === 'timed-out') throw new Error(`fetch timed out: ${url}`)
  if (Atomics.load(ctl, 1) !== 0) {
    const bytes = new Uint8Array(errorBuf)
    const end = bytes.indexOf(0)
    throw new Error(`fetch failed: ${url}: ${Buffer.from(bytes.subarray(0, end < 0 ? bytes.length : end)).toString()}`)
  }
  return file
}

export function syncFetchText(url, opts) {
  return readFileSync(syncFetchToFile(url, opts), 'utf8')
}
