// 本家フロントエンド（app.xrift.net）のページ内に注入する WebXR 実装（WebGL 版）
//
// contextBridge.executeInMainWorld() に関数ごと渡すので、この関数は自己完結していること
// （外側の変数・import を参照しない）。ネイティブ側へは preload が公開した
// window.__xriftOpenXR（OpenXR ブリッジ）だけを使う。
//
// three.js r185 の WebXRManager は XRWebGLBinding が無いと XRWebGLLayer 経路を使うので、
// Chromium 内蔵の XRWebGLBinding を隠し、XRWebGLLayer を自前実装する。
//   ページの three → XRWebGLLayer.framebuffer（MSAA）に両眼を横並びで描画
//   → 解決 + 上下反転の blit → PBO へ非同期 readPixels → フェンス待ち → OpenXR スワップチェーン

module.exports = function installXRiftWebXR() {
  const B = window.__xriftOpenXR
  if (!B || window.__xriftWebXRInstalled) return
  window.__xriftWebXRInstalled = true
  const log = (...a) => console.log('%c[XRift VR]', 'color:#a07cff', ...a)

  // ------------------------------------------------------------ 数学
  function quatToMat(p, q, out = new Float32Array(16)) {
    const x = q[0], y = q[1], z = q[2], w = q[3]
    const x2 = x + x, y2 = y + y, z2 = z + z
    const xx = x * x2, xy = x * y2, xz = x * z2, yy = y * y2, yz = y * z2, zz = z * z2
    const wx = w * x2, wy = w * y2, wz = w * z2
    out[0] = 1 - (yy + zz); out[1] = xy + wz; out[2] = xz - wy; out[3] = 0
    out[4] = xy - wz; out[5] = 1 - (xx + zz); out[6] = yz + wx; out[7] = 0
    out[8] = xz + wy; out[9] = yz - wx; out[10] = 1 - (xx + yy); out[11] = 0
    out[12] = p[0]; out[13] = p[1]; out[14] = p[2]; out[15] = 1
    return out
  }
  function invertRigid(m) {
    const o = new Float32Array(16)
    o[0] = m[0]; o[1] = m[4]; o[2] = m[8]
    o[4] = m[1]; o[5] = m[5]; o[6] = m[9]
    o[8] = m[2]; o[9] = m[6]; o[10] = m[10]
    o[12] = -(m[0] * m[12] + m[1] * m[13] + m[2] * m[14])
    o[13] = -(m[4] * m[12] + m[5] * m[13] + m[6] * m[14])
    o[14] = -(m[8] * m[12] + m[9] * m[13] + m[10] * m[14])
    o[15] = 1
    return o
  }
  function multiply(a, b) {
    const r = new Float32Array(16)
    for (let c = 0; c < 4; c++)
      for (let k = 0; k < 4; k++)
        r[c * 4 + k] = a[k] * b[c * 4] + a[4 + k] * b[c * 4 + 1] + a[8 + k] * b[c * 4 + 2] + a[12 + k] * b[c * 4 + 3]
    return r
  }
  function matToPosQuat(m) {
    const m00 = m[0], m01 = m[4], m02 = m[8], m10 = m[1], m11 = m[5], m12 = m[9], m20 = m[2], m21 = m[6], m22 = m[10]
    const tr = m00 + m11 + m22
    let x, y, z, w
    if (tr > 0) {
      const s = 0.5 / Math.sqrt(tr + 1); w = 0.25 / s; x = (m21 - m12) * s; y = (m02 - m20) * s; z = (m10 - m01) * s
    } else if (m00 > m11 && m00 > m22) {
      const s = 2 * Math.sqrt(1 + m00 - m11 - m22); w = (m21 - m12) / s; x = 0.25 * s; y = (m01 + m10) / s; z = (m02 + m20) / s
    } else if (m11 > m22) {
      const s = 2 * Math.sqrt(1 + m11 - m00 - m22); w = (m02 - m20) / s; x = (m01 + m10) / s; y = 0.25 * s; z = (m12 + m21) / s
    } else {
      const s = 2 * Math.sqrt(1 + m22 - m00 - m11); w = (m10 - m01) / s; x = (m02 + m20) / s; y = (m12 + m21) / s; z = 0.25 * s
    }
    return { pos: [m[12], m[13], m[14]], quat: [x, y, z, w] }
  }
  // WebGL（クリップ深度 [-1, 1]）用の非対称投影
  function projectionFromFov(fov, near, far) {
    const l = Math.tan(fov[0]), r = Math.tan(fov[1]), u = Math.tan(fov[2]), d = Math.tan(fov[3])
    const w = r - l, h = u - d
    const o = new Float32Array(16)
    o[0] = 2 / w; o[5] = 2 / h; o[8] = (r + l) / w; o[9] = (u + d) / h; o[11] = -1
    if (far === Infinity) { o[10] = -1; o[14] = -2 * near } else { o[10] = -(far + near) / (far - near); o[14] = -(2 * far * near) / (far - near) }
    return o
  }
  const IDENTITY = new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1])

  // ------------------------------------------------------------ 基本型
  class DOMPointRO { constructor(x = 0, y = 0, z = 0, w = 1) { this.x = x; this.y = y; this.z = z; this.w = w } }

  class XRRigidTransform {
    constructor(position, orientation) {
      if (position instanceof Float32Array && position.length === 16) {
        this._m = position
        const { pos, quat } = matToPosQuat(position)
        this.position = new DOMPointRO(pos[0], pos[1], pos[2], 1)
        this.orientation = new DOMPointRO(quat[0], quat[1], quat[2], quat[3])
        return
      }
      position = position || {}
      orientation = orientation || {}
      const p = [position.x || 0, position.y || 0, position.z || 0]
      let q = [orientation.x || 0, orientation.y || 0, orientation.z || 0, orientation.w ?? 1]
      const len = Math.hypot(q[0], q[1], q[2], q[3]) || 1
      q = q.map((v) => v / len)
      this.position = new DOMPointRO(p[0], p[1], p[2], 1)
      this.orientation = new DOMPointRO(q[0], q[1], q[2], q[3])
      this._m = quatToMat(p, q)
    }
    get matrix() { return this._m }
    get inverse() { return new XRRigidTransform(invertRigid(this._m)) }
  }
  const matrixOf = (t) => (t && t._m) || (t && t.matrix ? Float32Array.from(t.matrix) : IDENTITY)

  class XRSpace extends EventTarget {
    constructor(session, fn) { super(); this._session = session; this._fn = fn }
    _poseInApp(frame) { return this._fn(frame) }
  }
  // 基準空間: base(frame) がアプリ空間（SteamVR の立位空間）でのこの空間の原点、offset はさらにずらす量
  class XRReferenceSpace extends XRSpace {
    constructor(session, type, base = () => IDENTITY, offset = IDENTITY) {
      super(session, (f) => multiply(base(f), offset))
      this.type = type
      this._base = base
      this._offset = offset
      this.onreset = null
    }
    getOffsetReferenceSpace(originOffset) {
      const s = new XRReferenceSpace(this._session, this.type, this._base, multiply(this._offset, matrixOf(originOffset)))
      return s
    }
  }
  class XRBoundedReferenceSpace extends XRReferenceSpace { get boundsGeometry() { return [] } }

  class XRView {
    constructor(eye, transform, projectionMatrix, index) {
      this.eye = eye; this.transform = transform; this.projectionMatrix = projectionMatrix
      this.recommendedViewportScale = 1; this.isFirstPersonObserver = false; this._index = index
    }
    requestViewportScale() {}
  }
  class XRPose { constructor(t, emulated = false) { this.transform = t; this.emulatedPosition = emulated; this.linearVelocity = null; this.angularVelocity = null } }
  class XRViewerPose extends XRPose { constructor(t, views, emulated) { super(t, emulated); this.views = views } }

  class XRFrame {
    constructor(session, d) { this.session = session; this.predictedDisplayTime = d.time; this._d = d; this.trackedAnchors = undefined }
    getViewerPose(ref) {
      const d = this._d
      if (!d.viewsValid) return null
      const inv = invertRigid(ref._poseInApp(this))
      const views = d.views.map((v, i) => new XRView(i === 0 ? 'left' : 'right', new XRRigidTransform(multiply(inv, v.matrix)), v.projection, i))
      return new XRViewerPose(new XRRigidTransform(multiply(inv, d.head)), views, !d.positionTracked)
    }
    getPose(space, base) {
      const s = space && space._poseInApp(this)
      const b = base && base._poseInApp(this)
      if (!s || !b) return null
      return new XRPose(new XRRigidTransform(multiply(invertRigid(b), s)))
    }
    getJointPose() { return null }
    fillPoses() { return false }
    fillJointRadii() { return false }
    getHitTestResults() { return [] }
    getHitTestResultsForTransientInput() { return [] }
    getLightEstimate() { return null }
    getDepthInformation() { return null }
  }

  // ------------------------------------------------------------ 入力
  const PROFILES = {
    '/interaction_profiles/oculus/touch_controller': ['oculus-touch-v3', 'oculus-touch', 'generic-trigger-squeeze-thumbstick'],
    '/interaction_profiles/meta/touch_controller_plus': ['meta-quest-touch-plus', 'oculus-touch-v3', 'generic-trigger-squeeze-thumbstick'],
    '/interaction_profiles/valve/index_controller': ['valve-index', 'generic-trigger-squeeze-touchpad-thumbstick'],
    '/interaction_profiles/htc/vive_controller': ['htc-vive', 'generic-trigger-squeeze-touchpad'],
    '/interaction_profiles/microsoft/motion_controller': ['microsoft-mixed-reality', 'generic-trigger-squeeze-touchpad-thumbstick'],
    '/interaction_profiles/khr/simple_controller': ['generic-trigger'],
  }
  class GamepadButtonLike { constructor() { this.pressed = false; this.touched = false; this.value = 0 } }
  class XRGamepad {
    constructor(hand) {
      this.id = ''; this.index = -1; this.connected = true; this.mapping = 'xr-standard'; this.timestamp = performance.now()
      this.buttons = Array.from({ length: 6 }, () => new GamepadButtonLike())
      this.axes = [0, 0, 0, 0]
      const pulse = (intensity, duration) => { B.haptics(hand, intensity, duration); return Promise.resolve(true) }
      this.hapticActuators = [{ type: 'vibration', pulse }]
      this.vibrationActuator = { type: 'dual-rumble', pulse, playEffect: (_t, p) => pulse(p?.strongMagnitude ?? 1, p?.duration ?? 100), reset: () => Promise.resolve('completed') }
    }
    _update(s) {
      const set = (i, v, pressed = v > 0.5) => { const b = this.buttons[i]; b.value = v; b.pressed = pressed; b.touched = pressed || v > 0.05 }
      set(0, s.trigger); set(1, s.squeeze); set(2, s.trackpadClick ? 1 : 0); set(3, s.thumbstickClick ? 1 : 0)
      set(4, s.primary ? 1 : 0); set(5, s.secondary ? 1 : 0)
      this.axes[0] = s.trackpad[0]; this.axes[1] = -s.trackpad[1]
      this.axes[2] = s.thumbstick[0]; this.axes[3] = -s.thumbstick[1]
      if (!s.hasThumbstick && s.hasTrackpad) { this.axes[2] = this.axes[0]; this.axes[3] = this.axes[1] }
      this.timestamp = performance.now()
    }
  }
  class XRInputSource {
    constructor(session, i, profile) {
      this.handedness = i === 0 ? 'left' : 'right'
      this.targetRayMode = 'tracked-pointer'
      this.profiles = PROFILES[profile] || ['generic-trigger-squeeze-thumbstick']
      this.hand = null
      this.gamepad = new XRGamepad(i)
      this.skipRendering = false
      this._profile = profile
      this.targetRaySpace = new XRSpace(session, (f) => (f._d.hands[i] && f._d.hands[i].aim) || null)
      this.gripSpace = new XRSpace(session, (f) => (f._d.hands[i] && f._d.hands[i].grip) || null)
    }
  }
  class XRInputSourceEvent extends Event { constructor(type, init) { super(type); this.frame = init.frame; this.inputSource = init.inputSource } }
  class XRInputSourcesChangeEvent extends Event { constructor(type, init) { super(type); this.session = init.session; this.added = init.added; this.removed = init.removed } }
  class XRSessionEvent extends Event { constructor(type, init) { super(type); this.session = init.session } }

  // ------------------------------------------------------------ XRWebGLLayer（横並び両眼）
  //
  // framebuffer は null（= キャンバスの既定フレームバッファ）。three はキャンバスを
  // framebufferWidth x framebufferHeight に広げ、左右の眼をビューポートで描き分ける。
  // 描き終わった同じタスクのうちに、preload が WebGPU でキャンバスを GPU 上でコピーして読み戻す。
  class XRWebGLLayer {
    static getNativeFramebufferScaleFactor() { return 1 }
    constructor(session, gl, init = {}) {
      if (!(session instanceof XRSession)) throw new TypeError('XRWebGLLayer: session is not an XRift OpenXR session')
      const info = session._info
      this._gl = gl
      this._session = session
      this._eyeW = info.width
      this._eyeH = info.height
      this.framebuffer = null
      this.framebufferWidth = info.width * 2
      this.framebufferHeight = info.height
      this.antialias = gl.getContextAttributes().antialias
      this.ignoreDepthValues = true
      this.fixedFoveation = 0
    }
    getViewport(view) {
      const i = view && view._index !== undefined ? view._index : view && view.eye === 'right' ? 1 : 0
      return { x: i * this._eyeW, y: 0, width: this._eyeW, height: this._eyeH }
    }
    _present(seq) {
      const c = this._gl.canvas
      if (c.width !== this.framebufferWidth || c.height !== this.framebufferHeight) {
        // three が setSize するまでの最初のフレームなど
        return null
      }
      return B.present(c, this._eyeW, this._eyeH, seq)
    }
    _destroy() {}
  }

  const channel = new MessageChannel()
  const waiters = []
  channel.port1.onmessage = () => { const w = waiters.shift(); w && w() }
  function yieldTask() { return new Promise((r) => { waiters.push(r); channel.port2.postMessage(0) }) }
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

  // ------------------------------------------------------------ XRSession
  class XRSession extends EventTarget {
    constructor(info, mode, enabledFeatures) {
      super()
      this._info = info
      this._callbacks = new Map()
      this._handle = 1
      this._ended = false
      this._running = false
      this._sources = [null, null]
      this._prev = [{ t: false, s: false }, { t: false, s: false }]
      this.mode = mode
      this.enabledFeatures = enabledFeatures
      this.environmentBlendMode = 'opaque'
      this.interactionMode = 'world-space'
      this.visibilityState = 'hidden'
      this.supportedFrameRates = null
      this.frameRate = undefined
      this.isSystemKeyboardSupported = false
      this.domOverlayState = null
      this.renderState = { depthNear: 0.1, depthFar: 1000, inlineVerticalFieldOfView: null, baseLayer: null }
      for (const k of ['end', 'inputsourceschange', 'select', 'selectstart', 'selectend', 'squeeze', 'squeezestart', 'squeezeend', 'visibilitychange', 'frameratechange']) this['on' + k] = null
      this._stats = { n: 0, t: performance.now(), cb: 0, rb: 0, wait: 0, maxCb: 0, slow: 0, chain: 0, chainN: 0 }
      // 低遅延: 描いたフレームを同じフレーム内に提出する（間に合わないフレームだけ次のフレームで出す）
      B.setLatencyMode && B.setLatencyMode(true)
      this._loop = this._run()
    }
    get inputSources() { return this._sources.filter(Boolean) }
    updateRenderState(s = {}) {
      if (s.layers && s.layers.length) throw new DOMException('layers are not supported', 'NotSupportedError')
      Object.assign(this.renderState, s)
    }
    updateTargetFrameRate() { return Promise.resolve() }
    async requestReferenceSpace(type) {
      // WebXR と同じく local / local-floor は「VR に入ったときの立ち位置と向き」が原点（local は目の高さ、
      // local-floor は床の高さ）。bounded-floor / unbounded は SteamVR のプレイエリア（立位空間）のまま
      if (type === 'viewer') return new XRReferenceSpace(this, 'viewer', (f) => f._d.head)
      if (type === 'local') return new XRReferenceSpace(this, type, () => this._localOrigin(true))
      if (type === 'local-floor') return new XRReferenceSpace(this, type, () => this._localOrigin(false))
      if (type === 'unbounded') return new XRReferenceSpace(this, type)
      if (type === 'bounded-floor') return new XRBoundedReferenceSpace(this, type)
      throw new DOMException(`reference space "${type}" is not supported`, 'NotSupportedError')
    }
    // 最初に頭の位置が取れたフレームで、立ち位置（水平位置）と向き（ヨーだけ）を記録する
    _captureOrigin(head) {
      if (this._origin) return
      const yaw = Math.atan2(head[8], head[10])
      const c = Math.cos(yaw), s = Math.sin(yaw)
      this._origin = { x: head[12], y: head[13], z: head[14], c, s }
    }
    _localOrigin(eyeLevel) {
      const o = this._origin
      if (!o) return IDENTITY
      return new Float32Array([o.c, 0, -o.s, 0, 0, 1, 0, 0, o.s, 0, o.c, 0, o.x, eyeLevel ? o.y : 0, o.z, 1])
    }
    requestAnimationFrame(cb) { const h = this._handle++; this._callbacks.set(h, cb); return h }
    cancelAnimationFrame(h) { this._callbacks.delete(h) }
    requestHitTestSource() { return Promise.reject(new DOMException('hit-test is not supported', 'NotSupportedError')) }
    requestHitTestSourceForTransientInput() { return this.requestHitTestSource() }
    async end() {
      if (this._ended) return
      B.requestExit()
      await Promise.race([this._loop, sleep(3000)])
      this._finish()
    }
    _emit(type, ev) { const h = this['on' + type]; if (typeof h === 'function') h.call(this, ev); this.dispatchEvent(ev) }

    async _run() {
      try {
        while (!this._ended) {
          for (const e of B.pollEvents()) {
            if (e.type === 'begin') this._running = true
            else if (e.type === 'end') this._running = false
            else if (e.type === 'exit') this._ended = true
            else if (e.type === 'state') this._state(e.detail)
          }
          if (this._ended) break
          if (!this._running) { await sleep(20); continue }
          await this._frame()
        }
      } catch (err) {
        console.error('[XRift VR] frame loop error', err)
      } finally {
        this._finish()
      }
    }
    _state(s) {
      const vis = s === 'focused' ? 'visible' : s === 'visible' ? 'visible-blurred' : 'hidden'
      if (vis !== this.visibilityState) { this.visibilityState = vis; this._emit('visibilitychange', new XRSessionEvent('visibilitychange', { session: this })) }
    }
    async _frame() {
      const tw0 = performance.now()
      const fs = await B.waitFrame()
      const frameStart = performance.now()
      this._stats.wait += frameStart - tw0
      const period = fs.predictedDisplayPeriodMs || 13.9
      B.beginFrame()
      const lv = B.locateViews()
      const input = B.syncInput()
      const near = this.renderState.depthNear, far = this.renderState.depthFar
      const views = lv.views.map((v) => ({ matrix: quatToMat(v.position, v.orientation), projection: projectionFromFov(v.fov, near, far) }))
      const a = lv.views[0].position, b = lv.views[1].position
      const head = quatToMat([(a[0] + b[0]) / 2, (a[1] + b[1]) / 2, (a[2] + b[2]) / 2], lv.views[0].orientation)
      const hands = input.map((h) => ({
        ...h,
        grip: h.grip ? quatToMat(h.grip.position, h.grip.orientation) : null,
        aim: h.aim ? quatToMat(h.aim.position, h.aim.orientation) : null,
      }))
      if (lv.valid) this._captureOrigin(head)
      const frame = new XRFrame(this, { time: performance.now(), viewsValid: lv.valid, positionTracked: lv.positionTracked, views, head, hands })
      this._inputs(hands, frame)
      const layer = this.renderState.baseLayer
      if (fs.shouldRender && lv.valid && layer) {
        const cbs = [...this._callbacks.values()]
        this._callbacks.clear()
        const t0 = performance.now()
        for (const cb of cbs) {
          try { cb(t0, frame) } catch (err) { console.error('[XRift VR] XR frame callback error', err) }
        }
        const t1 = performance.now()
        // コールバックと同じタスクのうちに渡す（キャンバスの描画内容が有効なうちに GPU コピーを積む）。
        // 読み戻しとスワップチェーンへの書き込みは裏で進み、終わった画像が次以降の endFrame で出る
        // 読み戻しと転送は裏で進む。提出はネイティブのフレーム待ちスレッドが行う
        // （このフレームの転送が次のフレーム開始の少し前までに終われば遅れ 0、間に合わなければ最新の画像）
        layer._present(lv.seq)
        const st = this._stats
        st.cb += t1 - t0
        st.maxCb = Math.max(st.maxCb, t1 - t0)
        if (t1 - t0 > 12) st.slow++
        st.rb += performance.now() - t1
      }
      B.endFrame()
      const st = this._stats
      st.n++
      const now = performance.now()
      if (now - st.t > 5000) {
        log(`${((st.n * 1000) / (now - st.t)).toFixed(1)} fps | page render ${(st.cb / st.n).toFixed(1)}ms (max ${st.maxCb.toFixed(1)}, >12ms: ${st.slow}) | wait ${(st.wait / st.n).toFixed(1)}ms | enqueue ${(st.rb / st.n).toFixed(2)}ms | ${JSON.stringify(B.stats())}`)
        st.n = 0; st.cb = 0; st.rb = 0; st.wait = 0; st.maxCb = 0; st.slow = 0; st.chain = 0; st.chainN = 0; st.t = now
      }
    }
    _inputs(hands, frame) {
      const added = [], removed = []
      hands.forEach((h, i) => {
        const cur = this._sources[i]
        if (cur && (!h.connected || (h.profile && cur._profile !== h.profile))) { removed.push(cur); this._sources[i] = null }
        if (h.connected && !this._sources[i]) { const s = new XRInputSource(this, i, h.profile); this._sources[i] = s; added.push(s) }
      })
      if (added.length || removed.length) this._emit('inputsourceschange', new XRInputSourcesChangeEvent('inputsourceschange', { session: this, added, removed }))
      hands.forEach((h, i) => {
        const src = this._sources[i]
        if (!src) return
        src.gamepad._update(h)
        const p = this._prev[i]
        const t = h.trigger > 0.5, s = h.squeeze > 0.5
        const fire = (type) => this._emit(type, new XRInputSourceEvent(type, { frame, inputSource: src }))
        if (t && !p.t) fire('selectstart')
        if (!t && p.t) { fire('select'); fire('selectend') }
        if (s && !p.s) fire('squeezestart')
        if (!s && p.s) { fire('squeeze'); fire('squeezeend') }
        p.t = t; p.s = s
      })
    }
    _finish() {
      if (this._finished) return
      this._finished = true
      this._ended = true
      try { this.renderState.baseLayer && this.renderState.baseLayer._destroy() } catch {}
      try { B.destroy() } catch {}
      xr._active = null
      log('session ended')
      this._emit('end', new XRSessionEvent('end', { session: this }))
    }
  }

  // ------------------------------------------------------------ XRSystem
  const SUPPORTED = new Set(['viewer', 'local', 'local-floor', 'bounded-floor', 'unbounded'])
  class XRSystem extends EventTarget {
    constructor() { super(); this._active = null; this.ondevicechange = null; this.onsessiongranted = null }
    async isSessionSupported(mode) {
      if (mode === 'inline') return true
      if (mode !== 'immersive-vr') return false
      return B.probe().ok === true
    }
    async requestSession(mode, init = {}) {
      if (mode !== 'immersive-vr') throw new DOMException(`"${mode}" is not supported`, 'NotSupportedError')
      if (this._active) throw new DOMException('an immersive session is already active', 'InvalidStateError')
      for (const f of init.requiredFeatures || []) {
        if (!SUPPORTED.has(f)) throw new DOMException(`required feature "${f}" is not supported`, 'NotSupportedError')
      }
      const enabled = ['viewer', 'local', ...new Set([...(init.requiredFeatures || []), ...(init.optionalFeatures || [])].filter((f) => SUPPORTED.has(f)))]
      const gpu = await B.prepareGPU()
      if (!gpu.ok) throw new DOMException('WebGPU が使えません: ' + gpu.error, 'NotSupportedError')
      const info = B.createSession()
      if (!info || info.error) throw new DOMException('OpenXR: ' + (info && info.error), 'NotSupportedError')
      log(`${info.backend === "openvr" ? "OpenVR" : "OpenXR"}: ${info.runtimeName} / ${info.systemName} / ${info.width}x${info.height} per eye`)
      const s = new XRSession(info, mode, [...new Set(enabled)])
      this._active = s
      return s
    }
  }
  const xr = new XRSystem()

  // ------------------------------------------------------------ 差し替え
  Object.defineProperty(navigator, 'xr', { value: xr, configurable: true, enumerable: true })
  const replace = {
    XRSystem, XRSession, XRFrame, XRSpace, XRReferenceSpace, XRBoundedReferenceSpace, XRRigidTransform, XRView, XRPose,
    XRViewerPose, XRInputSource, XRWebGLLayer, XRInputSourceEvent, XRInputSourcesChangeEvent, XRSessionEvent,
  }
  for (const [k, v] of Object.entries(replace)) Object.defineProperty(window, k, { value: v, configurable: true, writable: true })
  // three は XRWebGLBinding があると Layers 経路を使うので隠す
  for (const k of ['XRWebGLBinding', 'XRGPUBinding', 'XRProjectionLayer', 'XRMediaBinding']) {
    try { delete window[k] } catch {}
    try { Object.defineProperty(window, k, { value: undefined, configurable: true, writable: true }) } catch {}
  }
  const makeXRCompatible = function () { return Promise.resolve() }
  for (const C of [window.WebGLRenderingContext, window.WebGL2RenderingContext]) if (C) C.prototype.makeXRCompatible = makeXRCompatible
  log('WebXR → OpenXR bridge installed')
}
