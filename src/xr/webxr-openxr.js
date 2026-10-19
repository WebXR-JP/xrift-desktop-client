// OpenXR → WebXR ブリッジ
//
// three.js の XRManager（WebGPU パス）が期待する WebXR Device API と WebXR/WebGPU Binding
// （XRGPUBinding / XRProjectionLayer / XRGPUSubImage）を、ネイティブの OpenXR セッションで実装する。
//
//   navigator.xr.requestSession('immersive-vr', { requiredFeatures: ['webgpu'] })
//   → renderer.xr.setSession(session)
//
// フレームごとの流れ:
//   xrWaitFrame（別スレッド）→ xrBeginFrame → xrLocateViews / 入力同期 → XRFrame を作って
//   rAF コールバック（three.js が XRGPUBinding のテクスチャに両眼を描く）
//   → 2 層の配列テクスチャを GPUBuffer に読み戻す → 各眼の OpenXR スワップチェーンへ書き込み → xrEndFrame

import { createRequire } from 'node:module'
import { XROutputPass } from './output-pass.js'

const require = createRequire(import.meta.url)

let native = null
export function loadNative() {
  if (!native) native = require('../../bin/xrift_openxr.node')
  return native
}

// ---------------------------------------------------------------- 数学ユーティリティ

function quatToMat(p, q, out = new Float32Array(16)) {
  const [x, y, z, w] = q
  const x2 = x + x, y2 = y + y, z2 = z + z
  const xx = x * x2, xy = x * y2, xz = x * z2
  const yy = y * y2, yz = y * z2, zz = z * z2
  const wx = w * x2, wy = w * y2, wz = w * z2
  out[0] = 1 - (yy + zz); out[1] = xy + wz; out[2] = xz - wy; out[3] = 0
  out[4] = xy - wz; out[5] = 1 - (xx + zz); out[6] = yz + wx; out[7] = 0
  out[8] = xz + wy; out[9] = yz - wx; out[10] = 1 - (xx + yy); out[11] = 0
  out[12] = p[0]; out[13] = p[1]; out[14] = p[2]; out[15] = 1
  return out
}

function invertRigid(m, out = new Float32Array(16)) {
  // 回転は転置、平行移動は -R^T t
  out[0] = m[0]; out[1] = m[4]; out[2] = m[8]; out[3] = 0
  out[4] = m[1]; out[5] = m[5]; out[6] = m[9]; out[7] = 0
  out[8] = m[2]; out[9] = m[6]; out[10] = m[10]; out[11] = 0
  const tx = m[12], ty = m[13], tz = m[14]
  out[12] = -(m[0] * tx + m[1] * ty + m[2] * tz)
  out[13] = -(m[4] * tx + m[5] * ty + m[6] * tz)
  out[14] = -(m[8] * tx + m[9] * ty + m[10] * tz)
  out[15] = 1
  return out
}

function multiply(a, b, out = new Float32Array(16)) {
  const r = new Float32Array(16)
  for (let c = 0; c < 4; c++) {
    for (let rr = 0; rr < 4; rr++) {
      r[c * 4 + rr] = a[rr] * b[c * 4] + a[4 + rr] * b[c * 4 + 1] + a[8 + rr] * b[c * 4 + 2] + a[12 + rr] * b[c * 4 + 3]
    }
  }
  out.set(r)
  return out
}

function matToPosQuat(m) {
  const pos = [m[12], m[13], m[14]]
  const m00 = m[0], m01 = m[4], m02 = m[8]
  const m10 = m[1], m11 = m[5], m12 = m[9]
  const m20 = m[2], m21 = m[6], m22 = m[10]
  const tr = m00 + m11 + m22
  let x, y, z, w
  if (tr > 0) {
    const s = 0.5 / Math.sqrt(tr + 1)
    w = 0.25 / s; x = (m21 - m12) * s; y = (m02 - m20) * s; z = (m10 - m01) * s
  } else if (m00 > m11 && m00 > m22) {
    const s = 2 * Math.sqrt(1 + m00 - m11 - m22)
    w = (m21 - m12) / s; x = 0.25 * s; y = (m01 + m10) / s; z = (m02 + m20) / s
  } else if (m11 > m22) {
    const s = 2 * Math.sqrt(1 + m11 - m00 - m22)
    w = (m02 - m20) / s; x = (m01 + m10) / s; y = 0.25 * s; z = (m12 + m21) / s
  } else {
    const s = 2 * Math.sqrt(1 + m22 - m00 - m11)
    w = (m10 - m01) / s; x = (m02 + m20) / s; y = (m12 + m21) / s; z = 0.25 * s
  }
  return { pos, quat: [x, y, z, w] }
}

/** OpenXR の非対称 FOV から WebGPU（深度 [0,1]）用の投影行列を作る */
function projectionFromFov(fov, near, far, out = new Float32Array(16)) {
  const l = Math.tan(fov[0]), r = Math.tan(fov[1]), u = Math.tan(fov[2]), d = Math.tan(fov[3])
  const w = r - l, h = u - d
  out.fill(0)
  out[0] = 2 / w
  out[5] = 2 / h
  out[8] = (r + l) / w
  out[9] = (u + d) / h
  out[11] = -1
  if (far === Infinity) {
    out[10] = -1
    out[14] = -near
  } else {
    out[10] = -far / (far - near)
    out[14] = -(far * near) / (far - near)
  }
  return out
}

// ---------------------------------------------------------------- WebXR 基本型

class DOMPointReadOnlyLike {
  constructor(x = 0, y = 0, z = 0, w = 1) {
    this.x = x; this.y = y; this.z = z; this.w = w
  }
}

export class XRRigidTransform {
  constructor(position = {}, orientation = {}) {
    if (position instanceof Float32Array || Array.isArray(position)) {
      // 内部用: 行列から
      this._matrix = Float32Array.from(position)
      const { pos, quat } = matToPosQuat(this._matrix)
      this.position = new DOMPointReadOnlyLike(pos[0], pos[1], pos[2], 1)
      this.orientation = new DOMPointReadOnlyLike(quat[0], quat[1], quat[2], quat[3])
      return
    }
    const p = [position.x ?? 0, position.y ?? 0, position.z ?? 0]
    let q = [orientation.x ?? 0, orientation.y ?? 0, orientation.z ?? 0, orientation.w ?? 1]
    const len = Math.hypot(...q) || 1
    q = q.map((v) => v / len)
    this.position = new DOMPointReadOnlyLike(p[0], p[1], p[2], 1)
    this.orientation = new DOMPointReadOnlyLike(q[0], q[1], q[2], q[3])
    this._matrix = quatToMat(p, q)
  }
  get matrix() { return this._matrix }
  get inverse() { return new XRRigidTransform(invertRigid(this._matrix)) }
}

export class XRSpace extends EventTarget {
  /** @param {(frame: XRFrame) => Float32Array|null} poseFn アプリ空間（OpenXR の app space）での姿勢行列 */
  constructor(session, poseFn) {
    super()
    this._session = session
    this._poseFn = poseFn
  }
  _poseInApp(frame) { return this._poseFn(frame) }
}

export class XRReferenceSpace extends XRSpace {
  constructor(session, type, originOffset = new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1])) {
    super(session, () => originOffset)
    this.type = type
    this._origin = originOffset
    this.onreset = null
  }
  getOffsetReferenceSpace(originOffset) {
    return new XRReferenceSpace(this._session, this.type, multiply(this._origin, originOffset.matrix))
  }
}

export class XRBoundedReferenceSpace extends XRReferenceSpace {
  get boundsGeometry() { return [] }
}

export class XRView {
  constructor(eye, transform, projectionMatrix, index) {
    this.eye = eye
    this.transform = transform
    this.projectionMatrix = projectionMatrix
    this.recommendedViewportScale = 1
    this.isFirstPersonObserver = false
    this._index = index
  }
  requestViewportScale() {}
}

export class XRPose {
  constructor(transform, emulatedPosition = false) {
    this.transform = transform
    this.emulatedPosition = emulatedPosition
    this.linearVelocity = null
    this.angularVelocity = null
  }
}

export class XRViewerPose extends XRPose {
  constructor(transform, views, emulatedPosition) {
    super(transform, emulatedPosition)
    this.views = views
  }
}

export class XRFrame {
  constructor(session, data) {
    this.session = session
    this.predictedDisplayTime = data.time
    this._data = data
    this._active = true
  }
  getViewerPose(referenceSpace) {
    const d = this._data
    if (!d.viewsValid) return null
    const inv = invertRigid(referenceSpace._poseInApp(this))
    const views = d.views.map((v, i) => {
      const m = multiply(inv, v.matrix)
      return new XRView(i === 0 ? 'left' : 'right', new XRRigidTransform(m), v.projection, i)
    })
    const head = multiply(inv, d.headMatrix)
    return new XRViewerPose(new XRRigidTransform(head), views, !d.positionTracked)
  }
  getPose(space, baseSpace) {
    const sm = space._poseInApp(this)
    const bm = baseSpace._poseInApp(this)
    if (!sm || !bm) return null
    return new XRPose(new XRRigidTransform(multiply(invertRigid(bm), sm)))
  }
  getJointPose() { return null }
  fillPoses() { return false }
  fillJointRadii() { return false }
}

// ---------------------------------------------------------------- 入力

const PROFILE_MAP = {
  '/interaction_profiles/oculus/touch_controller': ['oculus-touch-v3', 'oculus-touch', 'generic-trigger-squeeze-thumbstick'],
  '/interaction_profiles/meta/touch_controller_plus': ['meta-quest-touch-plus', 'oculus-touch-v3', 'generic-trigger-squeeze-thumbstick'],
  '/interaction_profiles/valve/index_controller': ['valve-index', 'generic-trigger-squeeze-touchpad-thumbstick'],
  '/interaction_profiles/htc/vive_controller': ['htc-vive', 'generic-trigger-squeeze-touchpad'],
  '/interaction_profiles/microsoft/motion_controller': ['microsoft-mixed-reality', 'generic-trigger-squeeze-touchpad-thumbstick'],
  '/interaction_profiles/khr/simple_controller': ['generic-trigger'],
}

class GamepadButton {
  constructor() { this.pressed = false; this.touched = false; this.value = 0 }
}

class XRGamepad {
  constructor(session, handIndex) {
    this.id = ''
    this.index = -1
    this.connected = true
    this.mapping = 'xr-standard'
    this.timestamp = performance.now()
    this.buttons = Array.from({ length: 6 }, () => new GamepadButton())
    this.axes = [0, 0, 0, 0]
    const pulse = (intensity, duration) => {
      session._native.applyHaptics(handIndex, intensity, duration)
      return Promise.resolve(true)
    }
    this.hapticActuators = [{ type: 'vibration', pulse }]
    this.vibrationActuator = { type: 'dual-rumble', playEffect: (_t, p) => pulse(p?.strongMagnitude ?? 1, p?.duration ?? 100), pulse, reset: () => Promise.resolve() }
  }
  _update(s) {
    const set = (i, value, pressed = value > 0.5) => {
      const b = this.buttons[i]
      b.value = value
      b.pressed = pressed
      b.touched = pressed || value > 0.05
    }
    set(0, s.trigger)
    set(1, s.squeeze)
    set(2, s.trackpadClick ? 1 : 0)
    set(3, s.thumbstickClick ? 1 : 0)
    set(4, s.primary ? 1 : 0)
    set(5, s.secondary ? 1 : 0)
    // WebXR は前（上）が -1
    this.axes[0] = s.trackpad[0]
    this.axes[1] = -s.trackpad[1]
    this.axes[2] = s.thumbstick[0]
    this.axes[3] = -s.thumbstick[1]
    // スティックが無いデバイス（Vive）はトラックパッドをスティック扱いにも出す
    if (!s.hasThumbstick && s.hasTrackpad) {
      this.axes[2] = this.axes[0]
      this.axes[3] = this.axes[1]
    }
    this.timestamp = performance.now()
  }
}

export class XRInputSource {
  constructor(session, handIndex, profilePath) {
    this.handedness = handIndex === 0 ? 'left' : 'right'
    this.targetRayMode = 'tracked-pointer'
    this.profiles = PROFILE_MAP[profilePath] ?? ['generic-trigger-squeeze-thumbstick']
    this.hand = null
    this.gamepad = new XRGamepad(session, handIndex)
    this.skipRendering = false
    this._handIndex = handIndex
    this._profilePath = profilePath
    this.targetRaySpace = new XRSpace(session, (frame) => frame._data.hands[handIndex]?.aimMatrix ?? null)
    this.gripSpace = new XRSpace(session, (frame) => frame._data.hands[handIndex]?.gripMatrix ?? null)
  }
}

class XRInputSourceEvent extends Event {
  constructor(type, init) {
    super(type)
    this.frame = init.frame
    this.inputSource = init.inputSource
  }
}

class XRInputSourcesChangeEvent extends Event {
  constructor(type, init) {
    super(type)
    this.session = init.session
    this.added = init.added
    this.removed = init.removed
  }
}

class XRSessionEvent extends Event {
  constructor(type, init) {
    super(type)
    this.session = init.session
  }
}

// ---------------------------------------------------------------- WebXR / WebGPU Binding

export class XRProjectionLayer {
  constructor(session, device, format) {
    const info = session._native.getInfo()
    this.textureWidth = info.width
    this.textureHeight = info.height
    this.textureArrayLength = 2
    this.ignoreDepthValues = true
    this.fixedFoveation = 0
    this.deltaPose = null
    this._format = format
    this._device = device
    // 2 層の配列テクスチャ。three はここに各眼を描き、セッションが読み戻して OpenXR へ渡す
    this._texture = device.createTexture({
      label: 'xr-projection-layer',
      size: [info.width, info.height, 2],
      format,
      // RENDER_ATTACHMENT | COPY_SRC | TEXTURE_BINDING
      usage: 0x10 | 0x01 | 0x04,
    })
  }
  destroy() {
    this._texture?.destroy()
    this._texture = null
  }
}

class XRGPUSubImage {
  constructor(layer, eyeIndex) {
    this.colorTexture = layer._texture
    this.depthStencilTexture = null
    this.motionVectorTexture = null
    this.imageIndex = eyeIndex
    this.colorTextureWidth = layer.textureWidth
    this.colorTextureHeight = layer.textureHeight
    this.viewport = { x: 0, y: 0, width: layer.textureWidth, height: layer.textureHeight }
    this._eye = eyeIndex
  }
  getViewDescriptor() {
    return { dimension: '2d', baseArrayLayer: this._eye, arrayLayerCount: 1, baseMipLevel: 0, mipLevelCount: 1 }
  }
}

export class XRGPUBinding {
  constructor(session, device) {
    if (!(session instanceof XRSession)) throw new TypeError('XRGPUBinding: not an OpenXR-backed session')
    this._session = session
    this._device = device
    session._device = device
  }
  get nativeProjectionScaleFactor() { return 1 }
  // three にはリニア HDR で描かせ、トーンマップ/sRGB 化は XROutputPass で行う
  getPreferredColorFormat() { return 'rgba16float' }
  createProjectionLayer(init = {}) {
    const format = init.colorFormat ?? this.getPreferredColorFormat()
    const layer = new XRProjectionLayer(this._session, this._device, format)
    this._session._projectionLayers.add(layer)
    return layer
  }
  getViewSubImage(layer, view) {
    return new XRGPUSubImage(layer, view._index ?? (view.eye === 'right' ? 1 : 0))
  }
  getSubImage(layer) { return new XRGPUSubImage(layer, 0) }
}

// ---------------------------------------------------------------- XRSession

export class XRSession extends EventTarget {
  constructor(nativeSession, mode, enabledFeatures, options) {
    super()
    this._native = nativeSession
    this._mode = mode
    this._device = null
    this._projectionLayers = new Set()
    this._callbacks = new Map()
    this._nextHandle = 1
    this._ended = false
    this._inputSources = [null, null]
    this._prevButtons = [{ trigger: false, squeeze: false }, { trigger: false, squeeze: false }]
    this._readback = null
    this._options = options
    this._stats = { frames: 0, rendered: 0, last: performance.now() }

    this.enabledFeatures = enabledFeatures
    this.environmentBlendMode = 'opaque'
    this.interactionMode = 'world-space'
    this.visibilityState = 'hidden'
    this.supportedFrameRates = null
    this.frameRate = undefined
    this.isSystemKeyboardSupported = false
    this.renderState = { depthNear: 0.1, depthFar: 1000, inlineVerticalFieldOfView: null, baseLayer: null, layers: [] }
    this.onend = null
    this.oninputsourceschange = null
    this.onselect = this.onselectstart = this.onselectend = null
    this.onsqueeze = this.onsqueezestart = this.onsqueezeend = null
    this.onvisibilitychange = null

    this._appSpace = new XRReferenceSpace(this, 'local-floor')
    this._loopPromise = this._runLoop()
  }

  get inputSources() { return this._inputSources.filter(Boolean) }

  updateRenderState(state = {}) {
    Object.assign(this.renderState, state)
  }

  updateTargetFrameRate() { return Promise.resolve() }

  async requestReferenceSpace(type) {
    switch (type) {
      case 'viewer':
        return new XRSpaceViewer(this)
      case 'local':
      case 'local-floor':
      case 'unbounded':
        return new XRReferenceSpace(this, type)
      case 'bounded-floor':
        return new XRBoundedReferenceSpace(this, type)
      default:
        throw new DOMException(`reference space "${type}" is not supported`, 'NotSupportedError')
    }
  }

  requestAnimationFrame(cb) {
    const h = this._nextHandle++
    this._callbacks.set(h, cb)
    return h
  }

  cancelAnimationFrame(h) {
    this._callbacks.delete(h)
  }

  async end() {
    if (this._ended) return
    this._native.requestExit()
    // ランタイムが STOPPING → EXITING を返してくるまで待つ（ループ側で end を発火）
    await this._loopPromise
  }

  // ------------------------------------------------------------ フレームループ

  async _runLoop() {
    const n = this._native
    try {
      while (!this._ended) {
        let running = this._running ?? false
        for (const ev of n.pollEvents()) {
          if (ev.type === 'begin') running = true
          else if (ev.type === 'end') running = false
          else if (ev.type === 'exit') this._ended = true
          else if (ev.type === 'state') this._setVisibility(ev.detail)
          else if (ev.type === 'profiles') this._profilesDirty = true
          else if (ev.type === 'error') console.error('[openxr]', ev.detail)
        }
        this._running = running
        if (this._ended) break
        if (!running) {
          await sleep(20)
          continue
        }
        await this._frame()
      }
    } catch (err) {
      console.error('[openxr] frame loop error:', err)
    } finally {
      this._finish()
    }
  }

  _setVisibility(state) {
    const vis = state === 'focused' ? 'visible' : state === 'visible' ? 'visible-blurred' : 'hidden'
    if (vis !== this.visibilityState) {
      this.visibilityState = vis
      const ev = new XRSessionEvent('visibilitychange', { session: this })
      this.onvisibilitychange?.(ev)
      this.dispatchEvent(ev)
    }
    if (this._options.verbose) console.log('[openxr] state:', state)
  }

  async _frame() {
    const n = this._native
    const t0 = performance.now()
    const fs = await n.waitFrame()
    const t1 = performance.now()
    n.beginFrame()
    const located = n.locateViews()
    const input = n.syncInput()

    const near = this.renderState.depthNear
    const far = this.renderState.depthFar
    const views = located.views.map((v) => ({
      matrix: quatToMat(v.position, v.orientation),
      projection: projectionFromFov(v.fov, near, far),
    }))
    // 頭の姿勢 = 両眼の中点 + 左眼の向き
    const lp = located.views[0].position, rp = located.views[1].position
    const headMatrix = quatToMat([(lp[0] + rp[0]) / 2, (lp[1] + rp[1]) / 2, (lp[2] + rp[2]) / 2], located.views[0].orientation)

    const hands = input.map((h) => ({
      ...h,
      gripMatrix: h.grip ? quatToMat(h.grip.position, h.grip.orientation) : null,
      aimMatrix: h.aim ? quatToMat(h.aim.position, h.aim.orientation) : null,
    }))
    const frame = new XRFrame(this, {
      time: performance.now(),
      viewsValid: located.valid,
      positionTracked: located.positionTracked,
      views,
      headMatrix,
      hands,
    })

    this._updateInputSources(hands, frame)

    const shouldRender = (fs.shouldRender || this._options.forceRender === true) && located.valid
    if (shouldRender) {
      const callbacks = [...this._callbacks.values()]
      this._callbacks.clear()
      const time = performance.now()
      for (const cb of callbacks) {
        try {
          cb(time, frame)
        } catch (err) {
          console.error('[webxr] animation frame callback error:', err)
        }
      }
      const t2 = performance.now()
      await this._copyToSwapchains()
      const t3 = performance.now()
      const p = (this._prof ??= { wait: 0, cb: 0, copy: 0, n: 0 })
      p.wait += t1 - t0
      p.cb += t2 - t1
      p.copy += t3 - t2
      p.n++
    } else {
      // 描かないフレームでもコールバックは次のフレームに持ち越す
    }
    frame._active = false
    const submitted = n.endFrame()

    const st = this._stats
    st.frames++
    if (submitted) st.rendered++
    const now = performance.now()
    if (this._options.verbose && now - st.last > 5000) {
      const p = this._prof
      const avg = p?.n ? ` | wait ${(p.wait / p.n).toFixed(1)}ms, render ${(p.cb / p.n).toFixed(1)}ms, readback ${(p.copy / p.n).toFixed(1)}ms` : ''
      console.log(`[openxr] ${(st.rendered / ((now - st.last) / 1000)).toFixed(1)} fps submitted, ${(st.frames / ((now - st.last) / 1000)).toFixed(1)} fps loop${avg}`)
      this._prof = null
      st.frames = st.rendered = 0
      st.last = now
    }
  }

  async _copyToSwapchains() {
    const layer = [...this._projectionLayers][0]
    const device = this._device
    if (!layer || !device || !layer._texture) return
    const w = layer.textureWidth
    const h = layer.textureHeight
    const bytesPerRow = Math.ceil((w * 4) / 256) * 256
    const layerBytes = bytesPerRow * h
    if (!this._readback || this._readback.size !== layerBytes * 2) {
      this._readback?.destroy()
      // MAP_READ | COPY_DST
      this._readback = device.createBuffer({ label: 'xr-readback', size: layerBytes * 2, usage: 0x0001 | 0x0008 })
    }
    const buf = this._readback
    const info = this._native.getInfo()
    const out = (this._outputPass ??= new XROutputPass(device))
    const cfg = this._outputProvider?.() ?? { toneMapping: 0, exposure: 1 }
    const enc = device.createCommandEncoder({ label: 'xr-readback' })
    out.encode(enc, layer._texture, buf, {
      width: w,
      height: h,
      bytesPerRow,
      toneMapping: cfg.toneMapping,
      exposure: cfg.exposure,
      bgra: info.colorFormat === 'bgra8unorm',
      // *_SRGB スワップチェーンならエンコード済みの値を書く（コンポジタがデコードする）
      encodeSRGB: info.srgbSwapchain || cfg.encodeSRGB === true,
    })
    device.queue.submit([enc.finish()])
    await buf.mapAsync(0x0001 /* GPUMapMode.READ */)
    const data = buf.getMappedRange()
    try {
      this._options.onReadback?.({ data, width: w, height: h, bytesPerRow, layerBytes, format: info.colorFormat })
      this._native.submit(0, data, 0, bytesPerRow)
      this._native.submit(1, data, layerBytes, bytesPerRow)
    } finally {
      buf.unmap()
    }
  }

  _updateInputSources(hands, frame) {
    const added = []
    const removed = []
    hands.forEach((h, i) => {
      const cur = this._inputSources[i]
      const want = h.connected
      if (cur && (!want || (h.profile && cur._profilePath !== h.profile))) {
        removed.push(cur)
        this._inputSources[i] = null
      }
      if (want && !this._inputSources[i]) {
        const src = new XRInputSource(this, i, h.profile)
        this._inputSources[i] = src
        added.push(src)
      }
    })
    if (added.length || removed.length) {
      const ev = new XRInputSourcesChangeEvent('inputsourceschange', { session: this, added, removed })
      this.oninputsourceschange?.(ev)
      this.dispatchEvent(ev)
    }

    // gamepad 更新と select / squeeze イベント
    hands.forEach((h, i) => {
      const src = this._inputSources[i]
      if (!src) return
      src.gamepad._update(h)
      const prev = this._prevButtons[i]
      const trig = h.trigger > 0.5
      const sq = h.squeeze > 0.5
      const fire = (type) => {
        const ev = new XRInputSourceEvent(type, { frame, inputSource: src })
        this['on' + type]?.(ev)
        this.dispatchEvent(ev)
      }
      if (trig && !prev.trigger) fire('selectstart')
      if (!trig && prev.trigger) {
        fire('select')
        fire('selectend')
      }
      if (sq && !prev.squeeze) fire('squeezestart')
      if (!sq && prev.squeeze) {
        fire('squeeze')
        fire('squeezeend')
      }
      prev.trigger = trig
      prev.squeeze = sq
    })
  }

  _finish() {
    if (this._finished) return
    this._finished = true
    this._ended = true
    for (const l of this._projectionLayers) l.destroy()
    this._projectionLayers.clear()
    this._readback?.destroy()
    this._readback = null
    this._outputPass?.destroy()
    this._outputPass = null
    try {
      this._native.destroy()
    } catch {}
    const ev = new XRSessionEvent('end', { session: this })
    this.onend?.(ev)
    this.dispatchEvent(ev)
  }
}

class XRSpaceViewer extends XRReferenceSpace {
  constructor(session) {
    super(session, 'viewer')
    this._poseFn = (frame) => frame._data.headMatrix
  }
  _poseInApp(frame) { return this._poseFn(frame) }
}

function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms))
}

// ---------------------------------------------------------------- XRSystem

export class XRSystem extends EventTarget {
  constructor(options = {}) {
    super()
    this._options = options
    this._active = null
    this.ondevicechange = null
  }

  async isSessionSupported(mode) {
    if (mode !== 'immersive-vr') return mode === 'inline'
    try {
      const n = loadNative()
      return (this._options.backend === 'openxr' ? n.probeRuntime() : n.probeOpenVR()).ok === true
    } catch {
      return false
    }
  }

  async requestSession(mode, init = {}) {
    if (mode !== 'immersive-vr') throw new DOMException(`session mode "${mode}" is not supported`, 'NotSupportedError')
    if (this._active) throw new DOMException('an immersive session is already active', 'InvalidStateError')
    const native = loadNative()
    const NativeSession = this._options.backend === 'openxr' ? native.OpenXRSession : native.OpenVRSession
    const required = init.requiredFeatures ?? []
    const optional = init.optionalFeatures ?? []
    const supported = new Set(['viewer', 'local', 'local-floor', 'bounded-floor', 'unbounded', 'webgpu'])
    for (const f of required) {
      if (!supported.has(f)) throw new DOMException(`required feature "${f}" is not supported`, 'NotSupportedError')
    }
    const enabled = ['viewer', 'local', ...new Set([...required, ...optional].filter((f) => supported.has(f)))]
    const nativeSession = new NativeSession({
      appName: this._options.appName ?? 'XRift OpenXR Client',
      resolutionScale: this._options.resolutionScale ?? 1,
      referenceSpace: this._options.referenceSpace ?? 'local-floor',
    })
    const info = nativeSession.getInfo()
    if (this._options.verbose !== false) {
      console.log(`[openxr] runtime: ${info.runtimeName} / ${info.systemName} / GPU: ${info.adapterName}`)
      console.log(`[openxr] eye: ${info.width}x${info.height} ${info.colorFormat}${info.srgbSwapchain ? ' (sRGB swapchain)' : ''}, space: ${info.referenceSpace}`)
    }
    const session = new XRSession(nativeSession, mode, [...new Set(enabled)], this._options)
    this._active = session
    session.addEventListener('end', () => {
      this._active = null
    })
    return session
  }
}

/** navigator.xr と XRGPUBinding などのグローバルを設定する */
export function installWebXR(options = {}) {
  const xr = new XRSystem(options)
  Object.defineProperty(globalThis.navigator, 'xr', { value: xr, configurable: true, writable: true })
  Object.assign(globalThis, {
    XRSystem,
    XRSession,
    XRFrame,
    XRSpace,
    XRReferenceSpace,
    XRBoundedReferenceSpace,
    XRRigidTransform,
    XRView,
    XRPose,
    XRViewerPose,
    XRInputSource,
    XRGPUBinding,
    XRProjectionLayer,
    XRInputSourceEvent,
    XRInputSourcesChangeEvent,
    XRSessionEvent,
  })
  return xr
}
