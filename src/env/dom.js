// Node 上で three.js / React Three Fiber / XRift ワールドを動かすための最小限のブラウザ環境
//
// - window / self / document / location / requestAnimationFrame
// - HTMLCanvasElement（2D は @napi-rs/canvas、'webgpu' は Dawn のテクスチャを返すシム）
// - HTMLImageElement / createImageBitmap / ImageBitmap（デコード結果を RGBA の .data として保持）
// - Worker（worker_threads 上で self/postMessage/onmessage を再現。DRACO / KTX2 のデコーダ用）
// - Audio / AudioContext / Video などはスタブ（エラーにならないことを優先）

import { createCanvas, loadImage, ImageData as NapiImageData, DOMMatrix, DOMPoint, Path2D } from '@napi-rs/canvas'
import { resolveObjectURL } from 'node:buffer'
import { Worker as NodeWorker } from 'node:worker_threads'
import { syncFetchText } from './sync-fetch.js'
import { SoftGLContext } from './softgl.js'

const g = globalThis

// ---------------------------------------------------------------- window / events

const windowEvents = new EventTarget()
g.window = g
g.self = g
g.addEventListener = windowEvents.addEventListener.bind(windowEvents)
g.removeEventListener = windowEvents.removeEventListener.bind(windowEvents)
g.dispatchEvent = windowEvents.dispatchEvent.bind(windowEvents)
g.innerWidth = 1280
g.innerHeight = 720
g.devicePixelRatio = 1
g.screen = { width: 1920, height: 1080, availWidth: 1920, availHeight: 1080 }
g.scrollX = g.scrollY = g.pageXOffset = g.pageYOffset = 0
g.isSecureContext = true

let loc = new URL('https://xrift.net/')
export function setLocation(href) {
  loc = new URL(href)
}
const location = {
  get href() { return loc.href },
  get origin() { return loc.origin },
  get protocol() { return loc.protocol },
  get host() { return loc.host },
  get hostname() { return loc.hostname },
  get port() { return loc.port },
  get pathname() { return loc.pathname },
  get search() { return loc.search },
  get hash() { return loc.hash },
  assign() {}, replace() {}, reload() {},
  toString() { return loc.href },
}
g.location = location

// 相対 URL / blob: URL を扱える fetch
const nodeFetch = g.fetch.bind(g)
g.fetch = async (input, init) => {
  let url = typeof input === 'string' ? input : input instanceof URL ? input.href : input?.url
  if (typeof url === 'string') {
    if (url.startsWith('blob:')) {
      const blob = resolveObjectURL(url)
      if (!blob) throw new TypeError(`Failed to fetch blob URL ${url}`)
      return new Response(await blob.arrayBuffer(), { status: 200, headers: { 'content-type': blob.type || 'application/octet-stream' } })
    }
    if (url.startsWith('data:')) return nodeFetch(url, init)
    if (!/^[a-z][a-z0-9+.-]*:/i.test(url)) {
      url = new URL(url, loc.href).href
      return nodeFetch(url, init)
    }
  }
  return nodeFetch(input, init)
}

// rAF（XR 中は XRSession が自前で回すので、ここはデスクトップ／起動前用）
let rafId = 0
const rafTimers = new Map()
g.requestAnimationFrame = (cb) => {
  const id = ++rafId
  rafTimers.set(id, setTimeout(() => {
    rafTimers.delete(id)
    cb(performance.now())
  }, 16))
  return id
}
g.cancelAnimationFrame = (id) => {
  clearTimeout(rafTimers.get(id))
  rafTimers.delete(id)
}
g.requestIdleCallback ??= (cb) => setTimeout(() => cb({ didTimeout: false, timeRemaining: () => 8 }), 1)
g.cancelIdleCallback ??= (id) => clearTimeout(id)

g.matchMedia = (query) => ({
  matches: false, media: query, onchange: null,
  addListener() {}, removeListener() {}, addEventListener() {}, removeEventListener() {}, dispatchEvent() { return false },
})
g.getComputedStyle = () => new Proxy({}, { get: (_, k) => (k === 'getPropertyValue' ? () => '' : '') })

class NoopObserver {
  constructor(cb) { this.cb = cb }
  observe() {} unobserve() {} disconnect() {} takeRecords() { return [] }
}
g.ResizeObserver ??= NoopObserver
g.IntersectionObserver ??= NoopObserver
g.MutationObserver ??= NoopObserver

class MemoryStorage {
  #m = new Map()
  get length() { return this.#m.size }
  key(i) { return [...this.#m.keys()][i] ?? null }
  getItem(k) { return this.#m.has(String(k)) ? this.#m.get(String(k)) : null }
  setItem(k, v) { this.#m.set(String(k), String(v)) }
  removeItem(k) { this.#m.delete(String(k)) }
  clear() { this.#m.clear() }
}
for (const name of ['localStorage', 'sessionStorage']) {
  Object.defineProperty(g, name, { value: new MemoryStorage(), configurable: true, writable: true })
}

// ---------------------------------------------------------------- DOM 要素

class Node extends EventTarget {
  constructor() {
    super()
    this.childNodes = []
    this.parentNode = null
  }
  get children() { return this.childNodes }
  get parentElement() { return this.parentNode }
  get firstChild() { return this.childNodes[0] ?? null }
  get lastChild() { return this.childNodes[this.childNodes.length - 1] ?? null }
  appendChild(child) {
    child.parentNode?.removeChild?.(child)
    this.childNodes.push(child)
    child.parentNode = this
    return child
  }
  append(...nodes) { for (const n of nodes) if (typeof n === 'object') this.appendChild(n) }
  prepend(...nodes) { for (const n of nodes.reverse()) if (typeof n === 'object') this.insertBefore(n, this.firstChild) }
  insertBefore(child, ref) {
    child.parentNode?.removeChild?.(child)
    const i = ref ? this.childNodes.indexOf(ref) : -1
    if (i < 0) this.childNodes.push(child)
    else this.childNodes.splice(i, 0, child)
    child.parentNode = this
    return child
  }
  removeChild(child) {
    const i = this.childNodes.indexOf(child)
    if (i >= 0) this.childNodes.splice(i, 1)
    child.parentNode = null
    return child
  }
  remove() { this.parentNode?.removeChild(this) }
  contains(n) {
    for (let c = n; c; c = c.parentNode) if (c === this) return true
    return false
  }
  get ownerDocument() { return g.document }
  get isConnected() { return true }
}

class Element extends Node {
  constructor(tagName = 'div') {
    super()
    this.tagName = tagName.toUpperCase()
    this.nodeName = this.tagName
    this.nodeType = 1
    this.style = new Proxy({ setProperty() {}, removeProperty() {}, getPropertyValue() { return '' } }, {})
    this.attributes = {}
    this.dataset = {}
    this.id = ''
    this.className = ''
    this.innerHTML = ''
    this.textContent = ''
    const classes = new Set()
    this.classList = {
      add: (...c) => c.forEach((x) => classes.add(x)),
      remove: (...c) => c.forEach((x) => classes.delete(x)),
      contains: (c) => classes.has(c),
      toggle: (c) => (classes.has(c) ? (classes.delete(c), false) : (classes.add(c), true)),
    }
  }
  setAttribute(k, v) { this.attributes[k] = String(v) }
  getAttribute(k) { return this.attributes[k] ?? null }
  removeAttribute(k) { delete this.attributes[k] }
  hasAttribute(k) { return k in this.attributes }
  getBoundingClientRect() {
    const w = this.width ?? g.innerWidth
    const h = this.height ?? g.innerHeight
    return { x: 0, y: 0, top: 0, left: 0, right: w, bottom: h, width: w, height: h }
  }
  get clientWidth() { return this.width ?? g.innerWidth }
  get clientHeight() { return this.height ?? g.innerHeight }
  get offsetWidth() { return this.clientWidth }
  get offsetHeight() { return this.clientHeight }
  focus() {} blur() {} click() {}
  querySelector() { return null }
  querySelectorAll() { return [] }
  getElementsByTagName() { return [] }
  setPointerCapture() {} releasePointerCapture() {} hasPointerCapture() { return false }
  requestPointerLock() {} requestFullscreen() { return Promise.resolve() }
}
class HTMLElement extends Element {}

// --------------------------------------------- 画像（デコード結果は { width, height, data(RGBA) }）

/** バイト列を RGBA ピクセルにデコードする */
export async function decodeImageBytes(bytes) {
  const buf = Buffer.isBuffer(bytes) ? bytes : bytes instanceof ArrayBuffer ? Buffer.from(bytes) : Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength)
  const img = await loadImage(buf)
  const w = img.width
  const h = img.height
  if (!w || !h) throw new Error('image decode failed')
  const c = createCanvas(w, h)
  const ctx = c.getContext('2d')
  ctx.drawImage(img, 0, 0)
  const id = ctx.getImageData(0, 0, w, h)
  return { width: w, height: h, data: new Uint8Array(id.data.buffer, id.data.byteOffset, id.data.byteLength) }
}

function flipRows(px) {
  const { width, height, data } = px
  const row = width * 4
  const out = new Uint8Array(data.length)
  for (let y = 0; y < height; y++) out.set(data.subarray(y * row, y * row + row), (height - 1 - y) * row)
  return { width, height, data: out }
}

async function bytesFromUrl(src) {
  const res = await g.fetch(src)
  if (!res.ok) throw new Error(`HTTP ${res.status} ${src}`)
  return new Uint8Array(await res.arrayBuffer())
}

class ImageBitmap {
  constructor(px) {
    this.width = px.width
    this.height = px.height
    this.data = px.data
  }
  close() { this.data = null }
}

class HTMLImageElement extends HTMLElement {
  constructor(width, height) {
    super('img')
    this._src = ''
    this._pixels = null
    this.complete = false
    this.crossOrigin = null
    this.decoding = 'auto'
    this.width = width ?? 0
    this.height = height ?? 0
    this.naturalWidth = 0
    this.naturalHeight = 0
    this.onload = null
    this.onerror = null
  }
  get src() { return this._src }
  set src(value) {
    this._src = String(value)
    this.complete = false
    const token = (this._token = Symbol())
    bytesFromUrl(this._src)
      .then(decodeImageBytes)
      .then((px) => {
        if (token !== this._token) return
        this._pixels = px
        this.width = this.naturalWidth = px.width
        this.height = this.naturalHeight = px.height
        this.complete = true
        const ev = new Event('load')
        this.onload?.(ev)
        this.dispatchEvent(ev)
      })
      .catch((err) => {
        if (token !== this._token) return
        this.complete = true
        const ev = new Event('error')
        ev.error = err
        this.onerror?.(ev)
        this.dispatchEvent(ev)
      })
  }
  decode() {
    if (this.complete && this._pixels) return Promise.resolve()
    return new Promise((resolve, reject) => {
      this.addEventListener('load', () => resolve(), { once: true })
      this.addEventListener('error', (e) => reject(e.error), { once: true })
    })
  }
}

// ---------------------------------------------------------------- Canvas

class GPUCanvasContextShim {
  constructor(canvas) {
    this.canvas = canvas
    this._config = null
    this._texture = null
  }
  configure(config) {
    this._config = config
    this._texture?.destroy()
    this._texture = null
  }
  unconfigure() {
    this._texture?.destroy()
    this._texture = null
    this._config = null
  }
  getConfiguration() { return this._config }
  getCurrentTexture() {
    const { device, format, usage = 0x10 } = this._config
    const w = Math.max(1, this.canvas.width | 0)
    const h = Math.max(1, this.canvas.height | 0)
    if (!this._texture || this._texture.width !== w || this._texture.height !== h) {
      this._texture?.destroy()
      // RENDER_ATTACHMENT | COPY_SRC | TEXTURE_BINDING
      this._texture = device.createTexture({ size: [w, h], format, usage: usage | 0x10 | 0x01 | 0x04, label: 'canvas' })
    }
    return this._texture
  }
}

class HTMLCanvasElement extends HTMLElement {
  constructor(width = 300, height = 150) {
    super('canvas')
    this._width = width
    this._height = height
    this._2d = null
    this._napi = null
    this._gpu = null
  }
  get width() { return this._width }
  set width(v) {
    this._width = Math.max(0, v | 0)
    if (this._napi) this._napi.width = Math.max(1, this._width)
  }
  get height() { return this._height }
  set height(v) {
    this._height = Math.max(0, v | 0)
    if (this._napi) this._napi.height = Math.max(1, this._height)
  }
  getContext(type) {
    if (type === '2d') {
      if (!this._napi) {
        this._napi = createCanvas(Math.max(1, this._width), Math.max(1, this._height))
        this._2d = this._napi.getContext('2d')
        this._2d.canvas = this
      }
      return this._2d
    }
    if (type === 'webgpu') return (this._gpu ??= new GPUCanvasContextShim(this))
    // troika-three-text の SDF アトラス用（コピーと readPixels だけの CPU 実装）
    if (type === 'webgl') return (this._softgl ??= new SoftGLContext(this))
    return null // webgl / webgl2 は無い
  }
  /** three.js へのアップロード用 RGBA */
  _getPixels() {
    if (this._softgl) return this._softgl._getPixels()
    if (!this._2d) return { width: Math.max(1, this._width), height: Math.max(1, this._height), data: new Uint8Array(Math.max(1, this._width) * Math.max(1, this._height) * 4) }
    const id = this._2d.getImageData(0, 0, this._napi.width, this._napi.height)
    return { width: id.width, height: id.height, data: new Uint8Array(id.data.buffer, id.data.byteOffset, id.data.byteLength) }
  }
  toDataURL(type = 'image/png') {
    this.getContext('2d')
    return this._napi.toDataURL(type)
  }
  toBlob(cb, type = 'image/png') {
    this.getContext('2d')
    cb(new Blob([this._napi.toBuffer(type)], { type }))
  }
  transferControlToOffscreen() { return this }
  captureStream() { return { getTracks: () => [] } }
}
class OffscreenCanvas extends HTMLCanvasElement {
  constructor(w, h) {
    super(w, h)
  }
  convertToBlob({ type = 'image/png' } = {}) {
    this.getContext('2d')
    return Promise.resolve(new Blob([this._napi.toBuffer(type)], { type }))
  }
  transferToImageBitmap() { return new ImageBitmap(this._getPixels()) }
}

// ---------------------------------------------------------------- Media（スタブ）

class HTMLMediaElement extends HTMLElement {
  constructor(tag) {
    super(tag)
    this.src = ''
    this.currentTime = 0
    this.duration = NaN
    this.paused = true
    this.muted = false
    this.volume = 1
    this.loop = false
    this.autoplay = false
    this.playsInline = false
    this.crossOrigin = null
    this.readyState = 0
    this.playbackRate = 1
    this.buffered = { length: 0, start: () => 0, end: () => 0 }
  }
  play() { this.paused = false; return Promise.resolve() }
  pause() { this.paused = true }
  load() {}
  canPlayType() { return '' }
  addTextTrack() { return {} }
}
class HTMLVideoElement extends HTMLMediaElement {
  constructor() {
    super('video')
    this.videoWidth = 0
    this.videoHeight = 0
  }
  requestVideoFrameCallback() { return 0 }
  cancelVideoFrameCallback() {}
}
class HTMLAudioElement extends HTMLMediaElement {
  constructor(src) {
    super('audio')
    if (src) this.src = src
  }
}

// WebAudio のスタブ（音は出ないが three の AudioListener / ワールドのコードが落ちない）
class FakeAudioParam {
  constructor(v = 0) { this.value = v; this.defaultValue = v }
  setValueAtTime(v) { this.value = v; return this }
  linearRampToValueAtTime(v) { this.value = v; return this }
  exponentialRampToValueAtTime(v) { this.value = v; return this }
  setTargetAtTime(v) { this.value = v; return this }
  cancelScheduledValues() { return this }
}
class FakeAudioNode extends EventTarget {
  constructor(ctx) {
    super()
    this.context = ctx
    this.gain = new FakeAudioParam(1)
    this.frequency = new FakeAudioParam(350)
    this.Q = new FakeAudioParam(1)
    this.detune = new FakeAudioParam(0)
    this.playbackRate = new FakeAudioParam(1)
    for (const k of ['positionX', 'positionY', 'positionZ', 'orientationX', 'orientationY', 'orientationZ']) this[k] = new FakeAudioParam(0)
    this.buffer = null
    this.loop = false
    this.onended = null
  }
  connect(n) { return n }
  disconnect() {}
  start() {} stop() {}
  setPosition() {} setOrientation() {}
  getFloatFrequencyData() {} getByteFrequencyData() {} getByteTimeDomainData() {} getFloatTimeDomainData() {}
}
class AudioContext extends EventTarget {
  constructor() {
    super()
    this.state = 'suspended'
    this.sampleRate = 48000
    this.destination = new FakeAudioNode(this)
    this.listener = new FakeAudioNode(this)
    this.startTime = performance.now()
  }
  get currentTime() { return (performance.now() - this.startTime) / 1000 }
  resume() { this.state = 'running'; return Promise.resolve() }
  suspend() { this.state = 'suspended'; return Promise.resolve() }
  close() { this.state = 'closed'; return Promise.resolve() }
  decodeAudioData(buf, ok) {
    const b = { duration: 0, length: 0, numberOfChannels: 2, sampleRate: 48000, getChannelData: () => new Float32Array(0) }
    ok?.(b)
    return Promise.resolve(b)
  }
  createBuffer(ch = 2, len = 0, rate = 48000) {
    return { duration: len / rate, length: len, numberOfChannels: ch, sampleRate: rate, getChannelData: () => new Float32Array(len) }
  }
}
for (const m of ['createGain', 'createPanner', 'createBufferSource', 'createMediaElementSource', 'createMediaStreamSource', 'createAnalyser', 'createBiquadFilter', 'createOscillator', 'createDynamicsCompressor', 'createConvolver', 'createStereoPanner', 'createDelay', 'createChannelSplitter', 'createChannelMerger']) {
  AudioContext.prototype[m] = function () { return new FakeAudioNode(this) }
}

// ---------------------------------------------------------------- document

const documentEvents = new EventTarget()
const html = new HTMLElement('html')
const head = new HTMLElement('head')
const body = new HTMLElement('body')
html.appendChild(head)
html.appendChild(body)

function createElement(tag) {
  switch (String(tag).toLowerCase()) {
    case 'canvas': return new HTMLCanvasElement()
    case 'img': return new HTMLImageElement()
    case 'video': return new HTMLVideoElement()
    case 'audio': return new HTMLAudioElement()
    default: return new HTMLElement(tag)
  }
}

g.document = {
  nodeType: 9,
  documentElement: html,
  head,
  body,
  readyState: 'complete',
  visibilityState: 'visible',
  hidden: false,
  pointerLockElement: null,
  fullscreenElement: null,
  activeElement: body,
  cookie: '',
  get location() { return location },
  createElement,
  createElementNS: (_ns, tag) => createElement(tag),
  createTextNode: (text) => Object.assign(new Node(), { nodeType: 3, textContent: text }),
  createDocumentFragment: () => new HTMLElement('fragment'),
  getElementById: () => null,
  getElementsByTagName: (t) => (t === 'head' ? [head] : t === 'body' ? [body] : []),
  getElementsByClassName: () => [],
  querySelector: () => null,
  querySelectorAll: () => [],
  addEventListener: documentEvents.addEventListener.bind(documentEvents),
  removeEventListener: documentEvents.removeEventListener.bind(documentEvents),
  dispatchEvent: documentEvents.dispatchEvent.bind(documentEvents),
  exitPointerLock() {},
  exitFullscreen() { return Promise.resolve() },
  hasFocus: () => true,
  fonts: { add() {}, delete() {}, has: () => false, load: () => Promise.resolve([]), check: () => true, ready: Promise.resolve(), addEventListener() {}, removeEventListener() {} },
}

// ---------------------------------------------------------------- createImageBitmap

async function sourceToPixels(source) {
  if (source instanceof ImageBitmap) return { width: source.width, height: source.height, data: source.data }
  if (source instanceof HTMLImageElement) {
    if (!source._pixels) await source.decode()
    return source._pixels
  }
  if (source instanceof HTMLCanvasElement) return source._getPixels()
  if (source instanceof NativeBlob) return decodeImageBytes(new Uint8Array(await source.arrayBuffer()))
  if (source?.data && source.width && source.height) {
    const d = source.data
    return { width: source.width, height: source.height, data: new Uint8Array(d.buffer, d.byteOffset, d.byteLength) }
  }
  throw new TypeError('createImageBitmap: unsupported source')
}

g.createImageBitmap = async (source, ...rest) => {
  const options = rest.length === 1 || rest.length === 5 ? rest[rest.length - 1] : undefined
  let px = await sourceToPixels(source)
  if (options?.imageOrientation === 'flipY') px = flipRows(px)
  return new ImageBitmap(px)
}

/** three.js の WebGPU バックエンドへ渡す RGBA を取り出す（出来なければ null） */
export function getImagePixels(image) {
  if (!image) return null
  if (image instanceof ImageBitmap) return image.data ? image : null
  if (image instanceof HTMLImageElement) return image._pixels
  if (image instanceof HTMLCanvasElement) return image._getPixels()
  if (image instanceof HTMLVideoElement) return null
  if (image.data && image.width && image.height) return image
  if (typeof image.getContext === 'function') {
    const ctx = image.getContext('2d')
    const id = ctx?.getImageData?.(0, 0, image.width, image.height)
    if (id) return { width: id.width, height: id.height, data: new Uint8Array(id.data.buffer, id.data.byteOffset, id.data.byteLength) }
  }
  return null
}

// ---------------------------------------------------------------- Worker（worker_threads ラッパー）

const WORKER_PRELUDE = `
const { parentPort } = require('node:worker_threads');
const __listeners = new Map();
globalThis.self = globalThis;
globalThis.window = undefined;
self.postMessage = (msg, transfer) => parentPort.postMessage(msg, Array.isArray(transfer) ? transfer.filter(t => t instanceof ArrayBuffer || (t && t.constructor && t.constructor.name === 'MessagePort')) : undefined);
self.addEventListener = (type, fn) => { if (!__listeners.has(type)) __listeners.set(type, new Set()); __listeners.get(type).add(fn); };
self.removeEventListener = (type, fn) => { __listeners.get(type)?.delete(fn); };
self.close = () => process.exit(0);
self.importScripts = (...urls) => { throw new Error('importScripts is not supported: ' + urls.join(', ')); };
parentPort.on('message', (data) => {
  const ev = { data, type: 'message' };
  if (typeof self.onmessage === 'function') self.onmessage(ev);
  for (const fn of __listeners.get('message') || []) fn(ev);
});
`

class Worker extends EventTarget {
  constructor(url, options = {}) {
    super()
    let source
    const href = typeof url === 'string' ? url : url.href
    if (href.startsWith('blob:')) {
      const blob = resolveObjectURL(href)
      if (!blob) throw new Error(`Worker: unknown blob URL ${href}`)
      // Blob#text() は非同期なので、内容を同期で読むために SharedArrayBuffer 経由ではなく
      // 事前に読み出してあるテキストを使う（createObjectURL フックで保存）
      source = blobTextCache.get(href)
      if (source === undefined) throw new Error('Worker: blob source is not available synchronously')
    } else if (href.startsWith('data:')) {
      const comma = href.indexOf(',')
      const meta = href.slice(0, comma)
      const body = href.slice(comma + 1)
      source = meta.endsWith(';base64') ? Buffer.from(body, 'base64').toString('utf8') : decodeURIComponent(body)
    } else {
      source = syncFetchText(new URL(href, loc.href).href)
    }
    // 空のスクリプトは「Worker が使えるか」の探り（troika など）。importScripts を使うライブラリは
    // この環境の Worker では動かないので、ここで失敗させてメインスレッド実行にフォールバックさせる
    if (!source.trim()) throw new Error('empty worker scripts are not supported in this environment')
    if (options.type === 'module') {
      console.warn('[dom] module workers are not supported; running as classic script')
    }
    this.onmessage = null
    this.onerror = null
    this._worker = new NodeWorker(WORKER_PRELUDE + '\n' + source, { eval: true })
    this._worker.on('message', (data) => {
      const ev = new MessageEvent('message', { data })
      this.onmessage?.(ev)
      this.dispatchEvent(ev)
    })
    this._worker.on('error', (err) => {
      const ev = new Event('error')
      ev.error = err
      ev.message = err?.message
      console.error('[worker]', err)
      this.onerror?.(ev)
      this.dispatchEvent(ev)
    })
    this._worker.unref()
  }
  postMessage(msg, transfer) {
    const list = Array.isArray(transfer) ? transfer : transfer?.transfer
    this._worker.postMessage(msg, list?.filter((t) => t instanceof ArrayBuffer))
  }
  terminate() { this._worker.terminate() }
}

// Worker(blobURL) を同期的に作れるよう、Blob のテキストを createObjectURL 時に控えておく。
// three の DRACOLoader / KTX2Loader は文字列から Blob を作るので、Blob 構築時の parts を覚える。
const blobTextCache = new Map()
const blobParts = new WeakMap()
const NativeBlob = g.Blob
class PatchedBlob extends NativeBlob {
  constructor(parts = [], options) {
    super(parts, options)
    if (parts.every((p) => typeof p === 'string')) blobParts.set(this, parts.join(''))
  }
}
g.Blob = PatchedBlob
const nativeCreateObjectURL = URL.createObjectURL.bind(URL)
URL.createObjectURL = (obj) => {
  const url = nativeCreateObjectURL(obj)
  if (blobParts.has(obj)) blobTextCache.set(url, blobParts.get(obj))
  return url
}
const nativeRevoke = URL.revokeObjectURL.bind(URL)
URL.revokeObjectURL = (url) => {
  blobTextCache.delete(url)
  nativeRevoke(url)
}


// ---------------------------------------------------------------- XMLHttpRequest（fetch で実装した非同期版）

class XMLHttpRequest extends EventTarget {
  static UNSENT = 0
  static OPENED = 1
  static HEADERS_RECEIVED = 2
  static LOADING = 3
  static DONE = 4
  constructor() {
    super()
    this.readyState = 0
    this.status = 0
    this.statusText = ''
    this.response = null
    this.responseText = ''
    this.responseType = ''
    this.responseURL = ''
    this.timeout = 0
    this.withCredentials = false
    this._headers = {}
    this._resHeaders = null
    this._abort = new AbortController()
    for (const k of ['onload', 'onerror', 'onprogress', 'onreadystatechange', 'onabort', 'onloadend', 'ontimeout']) this[k] = null
  }
  open(method, url) {
    this._method = method
    this._url = url
    this.readyState = 1
  }
  setRequestHeader(k, v) { this._headers[k] = v }
  overrideMimeType() {}
  getResponseHeader(k) { return this._resHeaders?.get(k) ?? null }
  getAllResponseHeaders() {
    return this._resHeaders ? [...this._resHeaders].map(([k, v]) => `${k}: ${v}`).join('\r\n') : ''
  }
  abort() {
    this._abort.abort()
    this._fire('abort')
  }
  _fire(type, extra = {}) {
    const ev = Object.assign(new Event(type), extra)
    this['on' + type]?.(ev)
    this.dispatchEvent(ev)
  }
  send(body) {
    g.fetch(this._url, { method: this._method, headers: this._headers, body, signal: this._abort.signal })
      .then(async (res) => {
        this.status = res.status
        this.statusText = res.statusText
        this.responseURL = res.url
        this._resHeaders = res.headers
        this.readyState = 2
        this._fire('readystatechange')
        const buf = await res.arrayBuffer()
        if (this.responseType === 'arraybuffer') this.response = buf
        else if (this.responseType === 'blob') this.response = new Blob([buf])
        else {
          this.responseText = Buffer.from(buf).toString('utf8')
          this.response = this.responseType === 'json' ? JSON.parse(this.responseText) : this.responseText
        }
        this.readyState = 4
        this._fire('progress', { loaded: buf.byteLength, total: buf.byteLength, lengthComputable: true })
        this._fire('readystatechange')
        this._fire('load')
        this._fire('loadend')
      })
      .catch((err) => {
        if (err?.name === 'AbortError') return
        this.readyState = 4
        this._fire('readystatechange')
        this._fire('error', { error: err })
        this._fire('loadend')
      })
  }
}

// ---------------------------------------------------------------- globals

Object.assign(g, {
  Node,
  Element,
  HTMLElement,
  HTMLCanvasElement,
  HTMLImageElement,
  HTMLVideoElement,
  HTMLAudioElement,
  HTMLMediaElement,
  OffscreenCanvas,
  ImageBitmap,
  ImageData: NapiImageData,
  DOMMatrix,
  DOMPoint,
  Path2D,
  Image: HTMLImageElement,
  Audio: HTMLAudioElement,
  AudioContext,
  webkitAudioContext: AudioContext,
  Worker,
  XMLHttpRequest,
  SVGElement: class SVGElement extends Element {},
  PointerEvent: g.PointerEvent ?? class PointerEvent extends Event {},
  MouseEvent: g.MouseEvent ?? class MouseEvent extends Event {},
  KeyboardEvent: g.KeyboardEvent ?? class KeyboardEvent extends Event {},
  WheelEvent: g.WheelEvent ?? class WheelEvent extends Event {},
  ProgressEvent: g.ProgressEvent ?? class ProgressEvent extends Event {
    constructor(type, init = {}) {
      super(type, init)
      this.lengthComputable = !!init.lengthComputable
      this.loaded = init.loaded ?? 0
      this.total = init.total ?? 0
    }
  },
  ErrorEvent: g.ErrorEvent ?? class ErrorEvent extends Event {
    constructor(type, init = {}) {
      super(type, init)
      this.message = init.message ?? ''
      this.error = init.error
    }
  },
  FontFace: class FontFace {
    constructor(family, source) { this.family = family; this.source = source; this.status = 'loaded' }
    load() { return Promise.resolve(this) }
  },
})

// Node 標準の navigator は getter なので、プロパティを足していく
const nav = g.navigator
for (const [k, v] of Object.entries({
  maxTouchPoints: 0,
  getGamepads: () => [],
  vendor: '',
  platform: 'Win32',
  mediaDevices: { getUserMedia: () => Promise.reject(new Error('not supported')), enumerateDevices: () => Promise.resolve([]), addEventListener() {}, removeEventListener() {} },
  clipboard: { writeText: () => Promise.resolve(), readText: () => Promise.resolve('') },
  vibrate: () => false,
})) {
  if (!(k in nav)) Object.defineProperty(nav, k, { value: v, configurable: true, writable: true })
}

