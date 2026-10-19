// preload（分離ワールド）: OpenXR ネイティブアドオンを読み込み、ページには最小限の XR 用ブリッジだけを公開する。
//
// ページ（app.xrift.net）にはユーザーが作ったワールドのコードも載るので、Node の機能は渡さない。
// 公開するのはフレームループに必要な関数（姿勢の取得、ピクセルの送信、振動など）だけ。

const { contextBridge } = require('electron')
const path = require('node:path')

const arg = (name, def) => {
  const hit = process.argv.find((a) => a.startsWith(`--xrift-${name}=`))
  return hit ? hit.slice(`--xrift-${name}=`.length) : def
}
const resolutionScale = Number(arg('scale', '1')) || 1
// HMD を被っていなくても描画・送信まで回す（動作確認用）
const forceRender = arg('force-render', '0') === '1'
// openvr（既定。VRChat などと同じ SteamVR ネイティブ API）/ openxr
const backend = arg('backend', 'openvr')
const addonPath = arg('addon', path.join(__dirname, '..', 'bin', 'xrift_openxr.node'))

let native = null
let session = null
const loadNative = () => (native ??= require(addonPath))

// ---- キャンバス → OpenXR（Chromium の WebGPU で GPU 上コピー → 非同期読み戻し）
// DOM 要素は分離ワールドとも共有されるので、ページのキャンバスをここで直接読める。
let gpuDevice = null
let staging = null // { texture, buffers: [GPUBuffer, GPUBuffer], w, h, bpr }
let ring = 0

async function prepareGPU() {
  if (gpuDevice) return { ok: true }
  if (!navigator.gpu) return { ok: false, error: 'navigator.gpu がありません' }
  const adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' })
  if (!adapter) return { ok: false, error: 'GPU アダプタがありません' }
  gpuDevice = await adapter.requestDevice()
  gpuDevice.lost.then((i) => {
    console.error('[XRift VR] WebGPU device lost', i.message)
    gpuDevice = null
    staging = null
  })
  return { ok: true }
}

function ensureStaging(w, h) {
  if (staging && staging.w === w && staging.h === h) return staging
  if (staging) {
    staging.texture.destroy()
    staging.buffers.forEach((b) => b.destroy())
  }
  const bpr = Math.ceil((w * 4) / 256) * 256
  staging = {
    w,
    h,
    bpr,
    texture: gpuDevice.createTexture({
      size: [w, h],
      format: 'rgba8unorm',
      usage: GPUTextureUsage.COPY_SRC | GPUTextureUsage.COPY_DST | GPUTextureUsage.RENDER_ATTACHMENT,
    }),
    buffers: [0, 1, 2].map(() => gpuDevice.createBuffer({ size: bpr * h, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST })),
  }
  return staging
}

// 読み戻し → スワップチェーン書き込みは前のフレームのものと直列につなぐ（描画とは並行に進む）
let chain = Promise.resolve()
const stats = {
  dropped: 0, n: 0, map: 0, sub: 0, wait: 0, notify: 0, frames: 0, detail: {}, onTime: 0, late: 0,
  submitTiming: { n: 0, renderToSubmit: 0, uploadToSubmit: 0, call: 0, first: 0, handoff: 0, calls: 0, lastSeq: 0 },
  frameTiming: {
    n: 0, appFrame: 0, preSubmitGpu: 0, submitFrame: 0, idle: 0, compositorGpu: 0,
    transfer: 0, clientInterval: 0, droppedFrames: 0, misPresented: 0, lastFrameIndex: 0,
  },
}

/**
 * 横並び両眼が描かれたキャンバスを OpenXR の左右スワップチェーンへ送る（非同期）。
 * キャンバスの内容が有効なうちに GPU コピーだけを積んで即座に戻る。
 * seq は locateViews() が返した、その画像を描いたときの姿勢の番号。
 * 先頭で取る QPC 時刻は「描画終了 → SteamVR の Submit」を測るためのタグ
 */
function present(canvas, eyeW, eyeH, seq) {
  if (!gpuDevice || !session) return null
  // 古いアドオンでは nowQpcMs が無い。その場合はタグなし（計測だけ無効）
  const tag = typeof session.nowQpcMs === 'function' ? session.nowQpcMs() : 0
  const w = canvas.width
  const h = canvas.height
  const st = ensureStaging(w, h)
  const buf = st.buffers.find((b) => b.mapState === 'unmapped' && !b.__busy)
  if (!buf) {
    stats.dropped++
    return null
  }
  buf.__busy = true
  // このフレームの画像を読み戻し中。フレーム待ちスレッドは締め切りまでその転送完了を待って提出する
  if (typeof session.markPending === 'function') session.markPending(true)
  gpuDevice.queue.copyExternalImageToTexture({ source: canvas }, { texture: st.texture }, [w, h])
  const enc = gpuDevice.createCommandEncoder()
  enc.copyTextureToBuffer({ texture: st.texture }, { buffer: buf, bytesPerRow: st.bpr, rowsPerImage: h }, [w, h])
  gpuDevice.queue.submit([enc.finish()])
  const t0 = performance.now()
  let t1 = 0
  const mapped = buf.mapAsync(GPUMapMode.READ).then(() => { t1 = performance.now() })
  const s = session
  chain = chain
    .then(() => mapped)
    .then(() => {
      const t2 = performance.now()
      stats.map += t1 - t0
      if (s !== session) return
      const data = buf.getMappedRange()
      if (typeof s.submitLatchedAsync === 'function') {
        // コピーはネイティブの別スレッドで行い、ページのメインスレッドを空ける
        return s.submitLatchedAsync(data, 0, eyeW * 4, st.bpr, seq, tag).then((r) => {
          stats.sub += performance.now() - t2
          stats.n++
          if (r) for (const [k, v] of Object.entries(r)) stats.detail[k] = (stats.detail[k] || 0) + v
        })
      }
      s.submitLatched(data, 0, eyeW * 4, st.bpr, seq)
      stats.sub += performance.now() - t2
      stats.n++
    })
    .catch((err) => {
      console.error('[XRift VR] present failed', err && err.message)
      if (s && typeof s.markPending === 'function') s.markPending(false)
    })
    .finally(() => {
      if (buf.mapState === 'mapped') buf.unmap()
      buf.__busy = false
    })
  // 転送（低遅延モードなら提出まで）が終わったら解決する
  return chain
}

const bridge = {
  prepareGPU,
  present,
  probe() {
    try {
      return backend === 'openxr' ? loadNative().probeRuntime() : loadNative().probeOpenVR()
    } catch (err) {
      return { ok: false, error: String(err && err.message) }
    }
  },
  createSession() {
    try {
      if (session) session.destroy()
      const n = loadNative()
      const Session = backend === 'openxr' ? n.OpenXRSession : n.OpenVRSession
      session = new Session({ appName: 'XRift VR', resolutionScale, referenceSpace: 'local-floor' })
      return session.getInfo()
    } catch (err) {
      session = null
      return { error: String(err && err.message) }
    }
  },
  pollEvents: () => (session ? session.pollEvents() : [{ type: 'exit', detail: 'no session' }]),
  async waitFrame() {
    const fs = await session.waitFrame()
    if (fs.fresh === true) stats.onTime++
    else if (fs.fresh === false) stats.late++
    stats.wait += fs.nativeWaitMs || 0
    stats.notify += fs.notifyDelayMs || 0
    stats.frames++
    // Submit はネイティブのフレーム待ちスレッドで起きるので、その計測値もここで集計する
    const st = fs.submitTiming
    if (st) {
      if (st.ran) {
        stats.submitTiming.calls++
        stats.submitTiming.call += st.submitCallMs || 0
        stats.submitTiming.first += st.submitFirstMs || 0
        stats.submitTiming.handoff += st.handoffMs || 0
        stats.submitTiming.lastSeq = st.seq || 0
      }
      if (st.valid) {
        stats.submitTiming.n++
        stats.submitTiming.renderToSubmit += st.renderToSubmitMs || 0
        stats.submitTiming.uploadToSubmit += st.uploadToSubmitMs || 0
      }
    }
    // SteamVR 自身のフレーム計測（Compositor_FrameTiming）。取れないランタイムでは来ない
    const ft = fs.frameTiming
    if (ft && ft.valid) {
      const f = stats.frameTiming
      f.n++
      f.appFrame += ft.appFrameMs
      f.preSubmitGpu += ft.preSubmitGpuMs
      f.submitFrame += ft.submitFrameMs
      f.idle += ft.compositorIdleCpuMs
      f.compositorGpu += ft.compositorRenderGpuMs
      f.transfer += ft.transferLatencyMs
      f.clientInterval += ft.clientFrameIntervalMs
      f.droppedFrames += ft.droppedFrames
      f.misPresented += ft.misPresented
      f.lastFrameIndex = ft.frameIndex
    }
    return { shouldRender: fs.shouldRender || forceRender, predictedDisplayPeriodMs: fs.predictedDisplayPeriodMs }
  },
  beginFrame: () => session.beginFrame(),
  locateViews: () => session.locateViews(),
  syncInput: () => session.syncInput(),
  // 両眼が横並びになった 1 枚の RGBA（上の行から）を受け取り、左右のスワップチェーンへ送る
  submitStereo(pixels, eyeWidth, rowPitch) {
    session.submit(0, pixels, 0, rowPitch)
    session.submit(1, pixels, eyeWidth * 4, rowPitch)
  },
  setLatencyMode(immediate) {
    if (session && typeof session.setLatencyMode === 'function') session.setLatencyMode(!!immediate)
  },
  stats() {
    const f = stats.frames || 1
    const st = stats.submitTiming
    const ft = stats.frameTiming
    const avg = (v, n) => (n ? +(v / n).toFixed(2) : null)
    const r = {
      dropped: stats.dropped,
      map: stats.n ? +(stats.map / stats.n).toFixed(1) : 0,
      submit: stats.n ? +(stats.sub / stats.n).toFixed(1) : 0,
      vrWait: +(stats.wait / f).toFixed(1),
      notifyDelay: +(stats.notify / f).toFixed(1),
      onTime: stats.onTime,
      late: stats.late,
      upload: Object.fromEntries(Object.entries(stats.detail).map(([k, v]) => [k, +(v / (stats.n || 1)).toFixed(2)])),
      submitTiming: st.calls ? {
        renderToSubmit: avg(st.renderToSubmit, st.n),
        uploadToSubmit: avg(st.uploadToSubmit, st.n),
        submitCall: avg(st.call, st.calls),
        submitFirst: avg(st.first, st.calls),
        handoff: avg(st.handoff, st.calls),
        n: st.n,
        lastSeq: st.lastSeq,
      } : null,
      frameTiming: ft.n ? {
        appFrame: avg(ft.appFrame, ft.n),
        preSubmitGpu: avg(ft.preSubmitGpu, ft.n),
        submitFrame: avg(ft.submitFrame, ft.n),
        compositorIdle: avg(ft.idle, ft.n),
        compositorGpu: avg(ft.compositorGpu, ft.n),
        transfer: avg(ft.transfer, ft.n),
        clientInterval: avg(ft.clientInterval, ft.n),
        droppedFrames: ft.droppedFrames,
        misPresented: ft.misPresented,
        frameIndex: ft.lastFrameIndex,
        n: ft.n,
      } : null,
    }
    stats.detail = {}
    stats.onTime = stats.late = 0
    stats.n = stats.map = stats.sub = stats.wait = stats.notify = stats.frames = 0
    st.n = st.renderToSubmit = st.uploadToSubmit = st.call = st.first = st.handoff = st.calls = 0
    st.lastSeq = 0
    ft.n = ft.appFrame = ft.preSubmitGpu = ft.submitFrame = ft.idle = ft.compositorGpu = 0
    ft.transfer = ft.clientInterval = ft.droppedFrames = ft.misPresented = 0
    ft.lastFrameIndex = 0
    return r
  },
  endFrame: () => session.endFrame(),
  haptics: (hand, amplitude, durationMs) => session && session.applyHaptics(hand, amplitude, durationMs),
  requestExit: () => session && session.requestExit(),
  destroy() {
    if (session) {
      try {
        session.destroy()
      } catch {}
      session = null
    }
  },
}

contextBridge.exposeInMainWorld('__xriftOpenXR', bridge)
// ページのスクリプトより先に navigator.xr を差し替える
contextBridge.executeInMainWorld({ func: require('./webxr-mainworld.cjs') })
