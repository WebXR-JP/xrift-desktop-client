/**
 * WebXR 実装。ページ内に注入する（`executeInMainWorld` で関数ごと渡すため、
 * この関数は自己完結していること。外側の変数・import を参照しない）。
 *
 * 旧ブリッジ `B` を新ブリッジに置き換えたものであり、API 形状・数学・イベントロジックを保つ。
 * 新ブリッジ `window.__xriftXR` は main プロセスが管理するセッションの描画要求
 * （`record`）と入力スナップショットを流す。`present` は無い（OSR が拾う）。
 *
 * @packageDocumentation
 */

/** ページへ注入する WebXR 実装の本体。preload から `executeInMainWorld` で呼ぶ。 */
export function installXRiftXR(): void {
  interface XrRecord {
    renderId: string;
    connEpoch: number;
    spaceEpoch: number;
    leftPos: number[];
    leftOri: number[];
    rightPos: number[];
    rightOri: number[];
    leftFov: number[];
    rightFov: number[];
    reqWidth: number;
    reqHeight: number;
    staleHint: boolean;
  }
  interface ControllerPose {
    valid: boolean;
    position: number[];
    orientation: number[];
  }
  interface InputHand {
    connected: boolean;
    profile: string;
    /** grip 空間の pose。無効時はシムの `gripSpace` が null を返す。 */
    grip: ControllerPose;
    /** aim 空間の pose。無効時はシムの `targetRaySpace` が null を返す。 */
    aim: ControllerPose;
    trigger: number;
    squeeze: number;
    thumbstickClick: boolean;
    trackpadClick: boolean;
    primary: boolean;
    secondary: boolean;
    menu: boolean;
    thumbstick: number[];
    trackpad: number[];
    hasThumbstick: boolean;
    hasTrackpad: boolean;
  }
  interface XRiftBridge {
    onRecord(cb: (rec: XrRecord) => void): void;
    onInput(cb: (snap: { left: InputHand; right: InputHand; latchLeft?: string; latchRight?: string }) => void): void;
    haptic(hand: number, intensity: number, durationMs: number): void;
    probe(): Promise<{ ok: boolean }>;
    sessionActive(active: boolean): void;
    // セッション初の制御ブロック描画の通知（main の Offer 開始用。任意）。
    // 無い橋（旧 preload・試験スタブ）でも描画駆動は変えない。
    stamped?(renderId: string): void;
  }
  const bridge = (window as unknown as { __xriftXR?: XRiftBridge }).__xriftXR;
  if (!bridge || (window as unknown as { __xriftXRInstalled?: boolean }).__xriftXRInstalled) return;
  const B: XRiftBridge = bridge;
  (window as unknown as { __xriftXRInstalled?: boolean }).__xriftXRInstalled = true;
  const log = (...a: unknown[]): void => console.log('%c[XRift XR]', 'color:#a07cff', ...a);

  // ------------------------------------------------------------ 制御ブロック
  // exe が画像から ID/pose を読むための画素内メタ。配置の正典は
  // `native/src/ipc/control_blocks.h` の kControlBlockOrg*（A=(64,64)、
  // B=(128,64)、マーカーは左 (160,8)・右 (右半分起点+160,8)、各 32x32）である。
  // DOM の overlay canvas に 2D で描き、Chromium の合成で OSR 共有テクスチャに載せる。
  // 描くのはページの描画コールバックを呼び終えた後であり、そのフレームに使った
  // record のタグに限る（painted 画像の内容とタグが同一 record に対応する）。
  // ページが rAF を呼ばなかった record では更新しない（描画していないため）。
  // 計算式は `src/control-codec.ts`（= C++ の PoseTag）と同一であり、
  // 変えたら両方を同時に更新する。注入関数は自己完結のため import できず同等品を持つ。
  const CONTROL_AX = 64, CONTROL_BX = 128, CONTROL_BY = 64, CONTROL_CELL = 4;
  const CONTROL_MLX = 160, CONTROL_MDX = 160, CONTROL_MY = 8, CONTROL_MS = 32;
  function quantizeByte(v: number, lo: number, hi: number): number {
    const t = ((v - lo) / (hi - lo)) * 255;
    if (!(t >= 0)) return 0;
    if (!(t <= 255)) return 255;
    return Math.floor(t + 0.5);
  }
  // C++ の PoseTag と同じバイト列（計 30B）の FNV-1a 下位 16bit。
  function poseTag(rec: XrRecord): number {
    const bytes: number[] = [];
    let id = BigInt(rec.renderId);
    for (let i = 0; i < 8; i++) { bytes.push(Number(id & 0xffn)); id >>= 8n; }
    const conn = rec.connEpoch >>> 0, space = rec.spaceEpoch >>> 0;
    for (let i = 0; i < 4; i++) bytes.push((conn >>> (i * 8)) & 0xff);
    for (let i = 0; i < 4; i++) bytes.push((space >>> (i * 8)) & 0xff);
    for (let i = 0; i < 3; i++) bytes.push(quantizeByte(rec.leftPos[i], -4, 4));
    for (let i = 0; i < 4; i++) bytes.push(quantizeByte(rec.leftOri[i], -1, 1));
    for (let i = 0; i < 3; i++) bytes.push(quantizeByte(rec.rightPos[i], -4, 4));
    for (let i = 0; i < 4; i++) bytes.push(quantizeByte(rec.rightOri[i], -1, 1));
    let hash = 2166136261 >>> 0;
    for (const b of bytes) { hash ^= b; hash = Math.imul(hash, 16777619) >>> 0; }
    return hash & 0xffff;
  }

  // ------------------------------------------------------------ 数学
  // 移植元と同一（回転・合成・非対称投影）。変えたら three の見え方が変わる。
  function quatToMat(p: number[], q: number[], out = new Float32Array(16)): Float32Array {
    const x = q[0], y = q[1], z = q[2], w = q[3];
    const x2 = x + x, y2 = y + y, z2 = z + z;
    const xx = x * x2, xy = x * y2, xz = x * z2, yy = y * y2, yz = y * z2, zz = z * z2;
    const wx = w * x2, wy = w * y2, wz = w * z2;
    out[0] = 1 - (yy + zz); out[1] = xy + wz; out[2] = xz - wy; out[3] = 0;
    out[4] = xy - wz; out[5] = 1 - (xx + zz); out[6] = yz + wx; out[7] = 0;
    out[8] = xz + wy; out[9] = yz - wx; out[10] = 1 - (xx + yy); out[11] = 0;
    out[12] = p[0]; out[13] = p[1]; out[14] = p[2]; out[15] = 1;
    return out;
  }
  function invertRigid(m: Float32Array): Float32Array {
    const o = new Float32Array(16);
    o[0] = m[0]; o[1] = m[4]; o[2] = m[8];
    o[4] = m[1]; o[5] = m[5]; o[6] = m[9];
    o[8] = m[2]; o[9] = m[6]; o[10] = m[10];
    o[12] = -(m[0] * m[12] + m[1] * m[13] + m[2] * m[14]);
    o[13] = -(m[4] * m[12] + m[5] * m[13] + m[6] * m[14]);
    o[14] = -(m[8] * m[12] + m[9] * m[13] + m[10] * m[14]);
    o[15] = 1;
    return o;
  }
  function multiply(a: Float32Array, b: Float32Array): Float32Array {
    const r = new Float32Array(16);
    for (let c = 0; c < 4; c++)
      for (let k = 0; k < 4; k++)
        r[c * 4 + k] = a[k] * b[c * 4] + a[4 + k] * b[c * 4 + 1] + a[8 + k] * b[c * 4 + 2] + a[12 + k] * b[c * 4 + 3];
    return r;
  }
  function matToPosQuat(m: Float32Array): { pos: number[]; quat: number[] } {
    const m00 = m[0], m01 = m[4], m02 = m[8], m10 = m[1], m11 = m[5], m12 = m[9], m20 = m[2], m21 = m[6], m22 = m[10];
    const tr = m00 + m11 + m22;
    let x: number, y: number, z: number, w: number;
    if (tr > 0) {
      const s = 0.5 / Math.sqrt(tr + 1); w = 0.25 / s; x = (m21 - m12) * s; y = (m02 - m20) * s; z = (m10 - m01) * s;
    } else if (m00 > m11 && m00 > m22) {
      const s = 2 * Math.sqrt(1 + m00 - m11 - m22); w = (m21 - m12) / s; x = 0.25 * s; y = (m01 + m10) / s; z = (m02 + m20) / s;
    } else if (m11 > m22) {
      const s = 2 * Math.sqrt(1 + m11 - m00 - m22); w = (m02 - m20) / s; x = (m01 + m10) / s; y = 0.25 * s; z = (m12 + m21) / s;
    } else {
      const s = 2 * Math.sqrt(1 + m22 - m00 - m11); w = (m10 - m01) / s; x = (m02 + m20) / s; y = (m12 + m21) / s; z = 0.25 * s;
    }
    return { pos: [m[12], m[13], m[14]], quat: [x, y, z, w] };
  }
  // WebGL（クリップ深度 [-1, 1]）用の非対称投影
  function projectionFromFov(fov: number[], near: number, far: number): Float32Array {
    const l = Math.tan(fov[0]), r = Math.tan(fov[1]), u = Math.tan(fov[2]), d = Math.tan(fov[3]);
    const w = r - l, h = u - d;
    const o = new Float32Array(16);
    o[0] = 2 / w; o[5] = 2 / h; o[8] = (r + l) / w; o[9] = (u + d) / h; o[11] = -1;
    if (far === Infinity) { o[10] = -1; o[14] = -2 * near; } else { o[10] = -(far + near) / (far - near); o[14] = -(2 * far * near) / (far - near); }
    return o;
  }
  const IDENTITY = new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);

  // ------------------------------------------------------------ 基本型（移植元と同一）
  class DOMPointRO {
    x: number; y: number; z: number; w: number;
    constructor(x = 0, y = 0, z = 0, w = 1) { this.x = x; this.y = y; this.z = z; this.w = w; }
  }

  class XRRigidTransform {
    position: DOMPointRO;
    orientation: DOMPointRO;
    _m: Float32Array;
    constructor(position?: unknown, orientation?: unknown) {
      if (position instanceof Float32Array && position.length === 16) {
        this._m = position;
        const { pos, quat } = matToPosQuat(position);
        this.position = new DOMPointRO(pos[0], pos[1], pos[2], 1);
        this.orientation = new DOMPointRO(quat[0], quat[1], quat[2], quat[3]);
        return;
      }
      const p = (position ?? {}) as { x?: number; y?: number; z?: number };
      const q0 = (orientation ?? {}) as { x?: number; y?: number; z?: number; w?: number };
      const pp = [p.x || 0, p.y || 0, p.z || 0];
      let qq = [q0.x || 0, q0.y || 0, q0.z || 0, q0.w ?? 1];
      const len = Math.hypot(qq[0], qq[1], qq[2], qq[3]) || 1;
      qq = qq.map((v) => v / len);
      this.position = new DOMPointRO(pp[0], pp[1], pp[2], 1);
      this.orientation = new DOMPointRO(qq[0], qq[1], qq[2], qq[3]);
      this._m = quatToMat(pp, qq);
    }
    get matrix(): Float32Array { return this._m; }
    get inverse(): XRRigidTransform { return new XRRigidTransform(invertRigid(this._m)); }
  }
  const matrixOf = (t: unknown): Float32Array => {
    const c = t as { _m?: Float32Array; matrix?: ArrayLike<number> } | null;
    return (c && c._m) || (c && c.matrix ? Float32Array.from(c.matrix) : IDENTITY);
  };

  class XRSpace extends EventTarget {
    _session: XRSession;
    _fn: (frame: XRFrame) => Float32Array | null;
    constructor(session: XRSession, fn: (frame: XRFrame) => Float32Array | null) { super(); this._session = session; this._fn = fn; }
    _poseInApp(frame: XRFrame): Float32Array | null { return this._fn(frame); }
  }
  // 基準空間: base(frame) がアプリ空間でのこの空間の原点、offset はさらにずらす量
  class XRReferenceSpace extends XRSpace {
    type: string;
    _base: (f: XRFrame) => Float32Array;
    _offset: Float32Array;
    onreset: ((ev: Event) => void) | null = null;
    constructor(session: XRSession, type: string, base: (f: XRFrame) => Float32Array = () => IDENTITY, offset: Float32Array = IDENTITY) {
      super(session, (f) => multiply(base(f), offset));
      this.type = type;
      this._base = base;
      this._offset = offset;
    }
    getOffsetReferenceSpace(originOffset: unknown): XRReferenceSpace {
      return new XRReferenceSpace(this._session, this.type, this._base, multiply(this._offset, matrixOf(originOffset)));
    }
  }
  class XRBoundedReferenceSpace extends XRReferenceSpace { get boundsGeometry(): never[] { return []; } }

  class XRView {
    eye: string;
    transform: XRRigidTransform;
    projectionMatrix: Float32Array;
    recommendedViewportScale = 1;
    isFirstPersonObserver = false;
    _index: number;
    constructor(eye: string, transform: XRRigidTransform, projectionMatrix: Float32Array, index: number) {
      this.eye = eye; this.transform = transform; this.projectionMatrix = projectionMatrix; this._index = index;
    }
    requestViewportScale(): void {}
  }
  class XRPose {
    transform: XRRigidTransform;
    emulatedPosition: boolean;
    linearVelocity = null;
    angularVelocity = null;
    constructor(t: XRRigidTransform, emulated = false) { this.transform = t; this.emulatedPosition = emulated; }
  }
  class XRViewerPose extends XRPose {
    views: XRView[];
    constructor(t: XRRigidTransform, views: XRView[], emulated: boolean) { super(t, emulated); this.views = views; }
  }

  interface FrameData {
    time: number;
    viewsValid: boolean;
    positionTracked: boolean;
    views: { matrix: Float32Array; projection: Float32Array }[];
    head: Float32Array;
    hands: HandPose[];
  }
  /** 行列化済みの手。pose が無効なとき gripMat/aimMat は null である。 */
  interface HandPose extends InputHand {
    gripMat: Float32Array | null;
    aimMat: Float32Array | null;
  }

  // pipe 由来の pose をアプリ空間の行列にする。無効・不正なときは null を返す
  // （`getPose` が null を返し、three はコントローラを出さない）。
  // 受信値は外部入力のため、有効旗・有限数・回転の長さを確認してから行列化する。
  function poseToMat(p: ControllerPose | null | undefined): Float32Array | null {
    if (!p || p.valid !== true) return null;
    const pos = p.position, ori = p.orientation;
    if (!Array.isArray(pos) || !Array.isArray(ori) || pos.length < 3 || ori.length < 4) return null;
    if (![pos[0], pos[1], pos[2], ori[0], ori[1], ori[2], ori[3]]
      .every((v) => typeof v === 'number' && Number.isFinite(v))) return null;
    const len = Math.hypot(ori[0], ori[1], ori[2], ori[3]);
    if (!(len > 1e-6)) return null;
    return quatToMat([pos[0], pos[1], pos[2]], [ori[0] / len, ori[1] / len, ori[2] / len, ori[3] / len]);
  }
  const NO_POSE: ControllerPose = { valid: false, position: [0, 0, 0], orientation: [0, 0, 0, 0] };

  class XRFrame {
    session: XRSession;
    predictedDisplayTime: number;
    _d: FrameData;
    trackedAnchors = undefined;
    /** このフレームを駆動した描画要求（テストページの画素内 ID 埋め込み用）。 */
    _xriftRecord: XrRecord | null = null;
    constructor(session: XRSession, d: FrameData) { this.session = session; this.predictedDisplayTime = d.time; this._d = d; }
    getViewerPose(ref: XRSpace): XRViewerPose | null {
      const d = this._d;
      if (!d.viewsValid) return null;
      const base = ref._poseInApp(this);
      if (!base) return null;
      const inv = invertRigid(base);
      const views = d.views.map((v, i) => new XRView(i === 0 ? 'left' : 'right', new XRRigidTransform(multiply(inv, v.matrix)), v.projection, i));
      return new XRViewerPose(new XRRigidTransform(multiply(inv, d.head)), views, !d.positionTracked);
    }
    getPose(space: XRSpace, base: XRSpace): XRPose | null {
      const s = space && space._poseInApp(this);
      const b = base && base._poseInApp(this);
      if (!s || !b) return null;
      return new XRPose(new XRRigidTransform(multiply(invertRigid(b), s)));
    }
    getJointPose(): null { return null; }
    fillPoses(): boolean { return false; }
    fillJointRadii(): boolean { return false; }
    getHitTestResults(): never[] { return []; }
    getHitTestResultsForTransientInput(): never[] { return []; }
    getLightEstimate(): null { return null; }
    getDepthInformation(): null { return null; }
  }

  // ------------------------------------------------------------ 入力（移植元と同一の形状）
  const PROFILES: Record<string, string[]> = {
    '/interaction_profiles/oculus/touch_controller': ['oculus-touch-v3', 'oculus-touch', 'generic-trigger-squeeze-thumbstick'],
    '/interaction_profiles/meta/touch_controller_plus': ['meta-quest-touch-plus', 'oculus-touch-v3', 'generic-trigger-squeeze-thumbstick'],
    '/interaction_profiles/valve/index_controller': ['valve-index', 'generic-trigger-squeeze-touchpad-thumbstick'],
    '/interaction_profiles/htc/vive_controller': ['htc-vive', 'generic-trigger-squeeze-touchpad'],
    '/interaction_profiles/microsoft/motion_controller': ['microsoft-mixed-reality', 'generic-trigger-squeeze-touchpad-thumbstick'],
    '/interaction_profiles/khr/simple_controller': ['generic-trigger'],
  };
  class GamepadButtonLike {
    pressed = false; touched = false; value = 0;
  }
  interface PadState {
    trigger: number; squeeze: number;
    trackpadClick: boolean; thumbstickClick: boolean;
    primary: boolean; secondary: boolean;
    trackpad: number[]; thumbstick: number[];
    hasThumbstick: boolean; hasTrackpad: boolean;
  }
  class XRGamepad {
    id = '';
    index = -1;
    connected = true;
    mapping = 'xr-standard';
    timestamp = performance.now();
    buttons: GamepadButtonLike[] = Array.from({ length: 6 }, () => new GamepadButtonLike());
    axes = [0, 0, 0, 0];
    hapticActuators: { type: string; pulse: (i: number, d: number) => Promise<boolean> }[];
    vibrationActuator: { type: string; pulse: (i: number, d: number) => Promise<boolean>; playEffect: (t: string, p?: { strongMagnitude?: number; duration?: number }) => Promise<boolean>; reset: () => Promise<string> };
    constructor(hand: number) {
      const pulse = (intensity: number, duration: number): Promise<boolean> => { B.haptic(hand, intensity, duration); return Promise.resolve(true); };
      this.hapticActuators = [{ type: 'vibration', pulse }];
      this.vibrationActuator = { type: 'dual-rumble', pulse, playEffect: (_t, p) => pulse(p?.strongMagnitude ?? 1, p?.duration ?? 100), reset: () => Promise.resolve('completed') };
    }
    _update(s: PadState): void {
      const set = (i: number, v: number, pressed = v > 0.5): void => { const b = this.buttons[i]; b.value = v; b.pressed = pressed; b.touched = pressed || v > 0.05; };
      set(0, s.trigger); set(1, s.squeeze); set(2, s.trackpadClick ? 1 : 0); set(3, s.thumbstickClick ? 1 : 0);
      set(4, s.primary ? 1 : 0); set(5, s.secondary ? 1 : 0);
      this.axes[0] = s.trackpad[0]; this.axes[1] = -s.trackpad[1];
      this.axes[2] = s.thumbstick[0]; this.axes[3] = -s.thumbstick[1];
      if (!s.hasThumbstick && s.hasTrackpad) { this.axes[2] = this.axes[0]; this.axes[3] = this.axes[1]; }
      this.timestamp = performance.now();
    }
  }
  class XRInputSource {
    handedness: string;
    targetRayMode = 'tracked-pointer';
    profiles: string[];
    hand = null;
    gamepad: XRGamepad;
    skipRendering = false;
    _profile: string;
    targetRaySpace: XRSpace;
    gripSpace: XRSpace;
    constructor(session: XRSession, i: number, profile: string) {
      this.handedness = i === 0 ? 'left' : 'right';
      this.profiles = PROFILES[profile] || ['generic-trigger-squeeze-thumbstick'];
      this.gamepad = new XRGamepad(i);
      this._profile = profile;
      this.targetRaySpace = new XRSpace(session, (f) => (f._d.hands[i] && f._d.hands[i].aimMat) || null);
      this.gripSpace = new XRSpace(session, (f) => (f._d.hands[i] && f._d.hands[i].gripMat) || null);
    }
  }
  class XRInputSourceEvent extends Event {
    frame: XRFrame; inputSource: XRInputSource;
    constructor(type: string, init: { frame: XRFrame; inputSource: XRInputSource }) { super(type); this.frame = init.frame; this.inputSource = init.inputSource; }
  }
  class XRInputSourcesChangeEvent extends Event {
    session: XRSession; added: XRInputSource[]; removed: XRInputSource[];
    constructor(type: string, init: { session: XRSession; added: XRInputSource[]; removed: XRInputSource[] }) { super(type); this.session = init.session; this.added = init.added; this.removed = init.removed; }
  }
  class XRSessionEvent extends Event {
    session: XRSession;
    constructor(type: string, init: { session: XRSession }) { super(type); this.session = init.session; }
  }

  // ------------------------------------------------------------ XRWebGLLayer（横並び両眼）
  //
  // framebuffer は null（= キャンバスの既定フレームバッファ）。three はキャンバスを
  // framebufferWidth x framebufferHeight に広げ、左右の眼をビューポートで描き分ける。
  // 提出は OSR の paint が拾うため、旧シムの `_present`（GPU コピー）は無い。
  class XRWebGLLayer {
    static getNativeFramebufferScaleFactor(): number { return 1; }
    _eyeW: number;
    _eyeH: number;
    framebuffer = null;
    framebufferWidth: number;
    framebufferHeight: number;
    antialias: boolean;
    ignoreDepthValues = true;
    fixedFoveation = 0;
    _canvas: HTMLCanvasElement;
    constructor(session: XRSession, gl: WebGLRenderingContext, _init: object = {}) {
      if (!(session instanceof XRSession)) throw new TypeError('XRWebGLLayer: session is not an XRift session');
      const info = session._info;
      this._eyeW = info.width;
      this._eyeH = info.height;
      this.framebufferWidth = info.width * 2;
      this.framebufferHeight = info.height;
      this.antialias = gl.getContextAttributes()?.antialias ?? false;
      this._canvas = gl.canvas as HTMLCanvasElement;
    }
    getViewport(view: XRView): { x: number; y: number; width: number; height: number } {
      const i = view && view._index !== undefined ? view._index : view && view.eye === 'right' ? 1 : 0;
      return { x: i * this._eyeW, y: 0, width: this._eyeW, height: this._eyeH };
    }
  }

  // ------------------------------------------------------------ 新ブリッジの購読
  // requestSession より前に届いた record/入力も捨てない（exe は起動時から送り続ける）。
  let latestRecord: XrRecord | null = null;
  let latestInput: { left: InputHand; right: InputHand } | null = null;
  // InputState のラッチ（その 1 秒間に押されたボタンの集合）。ページの HUD 表示用に
  // main 世界へ公開する（シム自体の入力駆動には使わない）。
  let latestLatch: { left: string; right: string } = { left: '-', right: '-' };
  (window as unknown as { __xriftLatch?: { left: string; right: string } }).__xriftLatch = latestLatch;
  // InputState の左右接続（HUD がセッション前でも接続を表示できるよう公開する）。
  // シム自体の入力駆動には使わない。
  let latestConn: { left: boolean; right: boolean; leftProfile: string; rightProfile: string } =
    { left: false, right: false, leftProfile: '', rightProfile: '' };
  (window as unknown as { __xriftInput?: { left: boolean; right: boolean; leftProfile: string; rightProfile: string } }).__xriftInput = latestConn;
  const recordWaiters: ((rec: XrRecord) => void)[] = [];
  B.onRecord((rec) => {
    latestRecord = rec;
    while (recordWaiters.length > 0) {
      const w = recordWaiters.shift();
      if (w) w(rec);
    }
  });
  B.onInput((snap) => {
    latestInput = snap;
    // ADR: 旧ブリッジ互換のため latch は任意扱いとし、欠けたら '-' を保つ。
    latestLatch = {
      left: typeof snap.latchLeft === 'string' ? snap.latchLeft : '-',
      right: typeof snap.latchRight === 'string' ? snap.latchRight : '-',
    };
    (window as unknown as { __xriftLatch?: { left: string; right: string } }).__xriftLatch = latestLatch;
    // 左右の接続・profile を HUD 用に公開する（欠けたら未接続扱い）。
    const handOf = (h: unknown): { connected: boolean; profile: string } => {
      if (h !== null && typeof h === 'object') {
        const r = h as Record<string, unknown>;
        return {
          connected: r.connected === true,
          profile: typeof r.profile === 'string' ? (r.profile as string) : '',
        };
      }
      return { connected: false, profile: '' };
    };
    const l = handOf((snap as unknown as Record<string, unknown>).left);
    const r = handOf((snap as unknown as Record<string, unknown>).right);
    latestConn = { left: l.connected, right: r.connected, leftProfile: l.profile, rightProfile: r.profile };
    (window as unknown as { __xriftInput?: { left: boolean; right: boolean; leftProfile: string; rightProfile: string } }).__xriftInput = latestConn;
  });
  function nextRecord(timeoutMs: number): Promise<XrRecord> {
    if (latestRecord) return Promise.resolve(latestRecord);
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        const i = recordWaiters.indexOf(done);
        if (i >= 0) recordWaiters.splice(i, 1);
        reject(new DOMException('RenderRequest が来ません（exe が起動しているか確認してください）', 'NotSupportedError'));
      }, timeoutMs);
      const done = (rec: XrRecord): void => { clearTimeout(timer); resolve(rec); };
      recordWaiters.push(done);
    });
  }

  interface SessionInfo {
    width: number; height: number;
    backend: string; runtimeName: string; systemName: string;
  }

  // ------------------------------------------------------------ XRSession
  // 状態は main が管理する。record が来るたびにフレームを進め、ページの rAF を呼ぶ。
  // フレーム終了の main への通知は無い（OSR の paint が提出を駆動するため）。
  class XRSession extends EventTarget {
    _info: SessionInfo;
    _callbacks = new Map<number, (t: number, f: XRFrame) => void>();
    _handle = 1;
    _ended = false;
    _finished = false;
    _sources: (XRInputSource | null)[] = [null, null];
    _prev = [{ t: false, s: false }, { t: false, s: false }];
    _origin: { x: number; y: number; z: number; c: number; s: number } | null = null;
    mode: string;
    enabledFeatures: string[];
    environmentBlendMode = 'opaque';
    interactionMode = 'world-space';
    visibilityState = 'visible';
    supportedFrameRates = null;
    frameRate = undefined;
    isSystemKeyboardSupported = false;
    domOverlayState = null;
    renderState: { depthNear: number; depthFar: number; inlineVerticalFieldOfView: null; baseLayer: XRWebGLLayer | null } = {
      depthNear: 0.1, depthFar: 1000, inlineVerticalFieldOfView: null, baseLayer: null,
    };
    onend: ((ev: Event) => void) | null = null;
    oninputsourceschange: ((ev: Event) => void) | null = null;
    onselect: ((ev: Event) => void) | null = null;
    onselectstart: ((ev: Event) => void) | null = null;
    onselectend: ((ev: Event) => void) | null = null;
    onsqueeze: ((ev: Event) => void) | null = null;
    onsqueezestart: ((ev: Event) => void) | null = null;
    onsqueezeend: ((ev: Event) => void) | null = null;
    onvisibilitychange: ((ev: Event) => void) | null = null;
    onframeratechange: ((ev: Event) => void) | null = null;
    _stats = { n: 0, t: performance.now(), cb: 0, maxCb: 0, slow: 0 };
    /** rAF コールバックへ1度でもフレームを渡したか。再入の二重駆動を防ぐ。 */
    _served = false;
    /** window.rAF ポンプの起動済み旗。hidden OSR の compositor を保つ。 */
    _pumpOn = false;
    /** record 受信ログの出力回数。最初の数回だけ renderId・眼・窓・canvas を出す。 */
    _recordLogs = 0;
    /** 最後にログした record の ID（rAF ポンプの重複を数えないため）。 */
    _lastLoggedId: string | null = null;
    /** 初回スタンプ通知の済み旗。main の Offer 開始用に 1 セッション 1 回だけ送る。 */
    _stampNotified = false;
    /** 制御ブロック用の overlay canvas（XR 開始時に WebGL canvas 上へ重ねる）。 */
    _overlay: HTMLCanvasElement | null = null;
    /** overlay の 2D 文脈（`_overlay` と対で持つ）。 */
    _overlayCtx: CanvasRenderingContext2D | null = null;
    _unsubscribe: () => void;
    constructor(info: SessionInfo, mode: string, enabledFeatures: string[]) {
      super();
      this._info = info;
      this.mode = mode;
      this.enabledFeatures = enabledFeatures;
      // ADR: 購読解除の持ち方は closing 時の leak 防止のためであり、record 駆動自体は変えない。
      let alive = true;
      const cb = (rec: XrRecord): void => { if (alive) this._onRecord(rec); };
      B.onRecord(cb);
      this._unsubscribe = () => { alive = false; };
    }
    get inputSources(): XRInputSource[] { return this._sources.filter((s): s is XRInputSource => s !== null); }
    updateRenderState(s: { layers?: unknown[]; depthNear?: number; depthFar?: number; baseLayer?: XRWebGLLayer | null } = {}): void {
      if (s.layers && s.layers.length) throw new DOMException('layers are not supported', 'NotSupportedError');
      Object.assign(this.renderState, s);
    }
    updateTargetFrameRate(): Promise<void> { return Promise.resolve(); }
    async requestReferenceSpace(type: string): Promise<XRReferenceSpace> {
      // 移植元と同一の意味。local / local-floor は VR に入ったときの立ち位置と向きが原点。
      if (type === 'viewer') return new XRReferenceSpace(this, 'viewer', (f) => f._d.head);
      if (type === 'local') return new XRReferenceSpace(this, type, () => this._localOrigin(true));
      if (type === 'local-floor') return new XRReferenceSpace(this, type, () => this._localOrigin(false));
      if (type === 'unbounded') return new XRReferenceSpace(this, type);
      if (type === 'bounded-floor') return new XRBoundedReferenceSpace(this, type);
      throw new DOMException(`reference space "${type}" is not supported`, 'NotSupportedError');
    }
    // 最初に頭の位置が取れたフレームで、立ち位置（水平位置）と向き（ヨーだけ）を記録する
    _captureOrigin(head: Float32Array): void {
      if (this._origin) return;
      const yaw = Math.atan2(head[8], head[10]);
      const c = Math.cos(yaw), s = Math.sin(yaw);
      this._origin = { x: head[12], y: head[13], z: head[14], c, s };
    }
    _localOrigin(eyeLevel: boolean): Float32Array {
      const o = this._origin;
      if (!o) return IDENTITY;
      return new Float32Array([o.c, 0, -o.s, 0, 0, 1, 0, 0, o.s, 0, o.c, 0, o.x, eyeLevel ? o.y : 0, o.z, 1]);
    }
    requestAnimationFrame(cb: (t: number, f: XRFrame) => void): number {
      const h = this._handle++;
      this._callbacks.set(h, cb);
      // exe はセッション開始前から送り続けるため、開始前に届いた record でも初回は進める。
      // baseLayer がある（setSession 済み）のに未配達のときだけ駆動する。
      if (!this._served && this.renderState.baseLayer && latestRecord) {
        this._onRecord(latestRecord);
      }
      // window.rAF ポンプを起こす。XR 駆動だけでは hidden OSR の compositor が止まり
      // paint が来ないため、rAF が BeginFrame を保つ（最新優先方式）。
      // record 到着と rAF のどちらでも進め、コールバックの取り合いは clear で直列化する。
      if (!this._pumpOn) {
        this._pumpOn = true;
        const pump = (): void => {
          if (this._ended) return;
          window.requestAnimationFrame(pump);
          if (latestRecord) this._onRecord(latestRecord);
        };
        window.requestAnimationFrame(pump);
      }
      return h;
    }
    cancelAnimationFrame(h: number): void { this._callbacks.delete(h); }
    requestHitTestSource(): Promise<never> { return Promise.reject(new DOMException('hit-test is not supported', 'NotSupportedError')); }
    requestHitTestSourceForTransientInput(): Promise<never> { return this.requestHitTestSource(); }
    async end(): Promise<void> {
      this._finish();
    }
    _emit(type: string, ev: Event): void {
      // ADR: 動的イベント名の振り分けのため unknown 経由で読む。入る値は on* の Event ハンドラに限る。
      const handler = (this as unknown as Record<string, unknown>)['on' + type];
      if (typeof handler === 'function') (handler as (ev: Event) => void).call(this, ev);
      this.dispatchEvent(ev);
    }

    // overlay canvas を WebGL canvas に重ね、record の制御ブロックを 2D で描く。
    // 描くのは制御領域だけであり、全画面の clear はしない（負荷最小・残像なし。
    // リサイズで消えても record ごとに再描画するため直る）。戻り値は描いた値。
    _drawControlBlocks(rec: XrRecord, canvas: HTMLCanvasElement, eyeW: number, eyeH: number): { id16: number; tag: number } {
      const targetW = eyeW * 2, targetH = eyeH;
      let ov = this._overlay;
      let ctx = this._overlayCtx;
      if (ov === null || ctx === null) {
        const el = document.createElement('canvas');
        const c = el.getContext('2d');
        if (c === null) throw new Error('overlay の 2d 文脈が取れません');
        el.setAttribute('data-xrift-control-blocks', '1');
        el.style.cssText = 'position:fixed;left:0;top:0;pointer-events:none;z-index:2147483647;';
        document.documentElement.appendChild(el);
        this._overlay = el;
        this._overlayCtx = c;
        ov = el;
        ctx = c;
      }
      // ADR: closure 内では let の絞り込みが残らないため const へ写す。
      const target: HTMLCanvasElement = ov;
      const g: CanvasRenderingContext2D = ctx;
      if (target.width !== targetW || target.height !== targetH) {
        target.width = targetW;
        target.height = targetH;
      }
      // WebGL canvas と画素単位で重ねる（ページの配置がずれても追従する）。
      const r = canvas.getBoundingClientRect();
      const st = target.style;
      const left = `${r.left}px`, top = `${r.top}px`, w = `${r.width}px`, h = `${r.height}px`;
      if (st.left !== left) st.left = left;
      if (st.top !== top) st.top = top;
      if (st.width !== w) st.width = w;
      if (st.height !== h) st.height = h;
      const id16 = Number(BigInt(rec.renderId) & 0xffffn);
      const tag = poseTag(rec);
      const block = (ox: number, value: number): void => {
        for (let bit = 0; bit < 16; bit++) {
          g.fillStyle = (((value >>> bit) & 1) === 1) ? '#fff' : '#000';
          g.fillRect(ox + (bit % 8) * CONTROL_CELL, CONTROL_BY + Math.floor(bit / 8) * CONTROL_CELL, CONTROL_CELL, CONTROL_CELL);
        }
      };
      block(CONTROL_AX, id16);
      block(CONTROL_BX, tag);
      g.fillStyle = '#f0f';
      g.fillRect(CONTROL_MLX, CONTROL_MY, CONTROL_MS, CONTROL_MS);
      g.fillStyle = '#0ff';
      g.fillRect(eyeW + CONTROL_MDX, CONTROL_MY, CONTROL_MS, CONTROL_MS);
      return { id16, tag };
    }

    _onRecord(rec: XrRecord): void {
      if (this._ended) return;
      const t0 = performance.now();
      const near = this.renderState.depthNear, far = this.renderState.depthFar;
      const views = [
        { matrix: quatToMat(rec.leftPos, rec.leftOri), projection: projectionFromFov(rec.leftFov, near, far) },
        { matrix: quatToMat(rec.rightPos, rec.rightOri), projection: projectionFromFov(rec.rightFov, near, far) },
      ];
      const a = rec.leftPos, b = rec.rightPos;
      const head = quatToMat([(a[0] + b[0]) / 2, (a[1] + b[1]) / 2, (a[2] + b[2]) / 2], rec.leftOri);
      // 入力 pose は pipe の InputState が運ぶ。有効なものだけ行列化し、
      // 無効時は null のままにする（`getPose` が null を返す）。
      const inp = latestInput;
      const hands: HandPose[] = [0, 1].map((i) => {
        const h = inp ? (i === 0 ? inp.left : inp.right) : null;
        const base: InputHand = h ?? {
          connected: false, profile: '', trigger: 0, squeeze: 0,
          thumbstickClick: false, trackpadClick: false, primary: false, secondary: false, menu: false,
          thumbstick: [0, 0], trackpad: [0, 0], hasThumbstick: false, hasTrackpad: false,
          grip: NO_POSE, aim: NO_POSE,
        };
        return { ...base, gripMat: poseToMat(base.grip), aimMat: poseToMat(base.aim) };
      });
      this._captureOrigin(head);
      const frame = new XRFrame(this, { time: performance.now(), viewsValid: true, positionTracked: true, views, head, hands });
      frame._xriftRecord = rec;
      this._inputs(hands, frame);
      const layer = this.renderState.baseLayer;
      if (layer) {
        // XR 中はキャンバスサイズをシムが所有する（ページの resize と競合させない）。
        // record の要求サイズを正とし、_info（初回 record の写し）と食い違えば追従する。
        // 要求が不正なときだけ _info に退避する。width 代入は描画内容を消すため、
        // 異なるときだけ行う。style も合わせないと CSS 側の寸法とずれる。
        const eyeW = rec.reqWidth > 0 ? rec.reqWidth : this._info.width;
        const eyeH = rec.reqHeight > 0 ? rec.reqHeight : this._info.height;
        if (eyeW !== this._info.width || eyeH !== this._info.height) {
          this._info.width = eyeW;
          this._info.height = eyeH;
        }
        const targetW = eyeW * 2, targetH = eyeH;
        const canvas = layer._canvas;
        if (canvas.width !== targetW || canvas.height !== targetH) {
          canvas.width = targetW;
          canvas.height = targetH;
        }
        // three の setSize がページ側で呼ばれても XR 中はシムの寸法を保つ。
        if (canvas.style.width !== `${targetW}px` || canvas.style.height !== `${targetH}px`) {
          canvas.style.width = `${targetW}px`;
          canvas.style.height = `${targetH}px`;
        }
        // 制御ブロック（renderId/pose タグ・左右マーカー）を overlay に描く。
        // 描くのはページの描画コールバックを呼び終えた後であり、そのフレームに
        // 使った record のタグに限る（painted 画像の内容とタグが同一 record に対応する）。
        // ページが rAF を呼ばなかった record ではタグを更新しない（描画していないため）。
        // 失敗しても XR 駆動は止めない。
        const cbs = [...this._callbacks.values()];
        this._callbacks.clear();
        // 再入（コールバック内の requestAnimationFrame）で二重駆動しないよう先に立てる。
        if (cbs.length > 0) this._served = true;
        for (const cb of cbs) {
          try { cb(t0, frame); } catch (err) { console.error('[XRift XR] XR frame callback error', err); }
        }
        let stamp: { id16: number; tag: number } | null = null;
        if (cbs.length > 0) {
          try {
            stamp = this._drawControlBlocks(rec, canvas, eyeW, eyeH);
            // ADR: 初回の描画完了を main へ知らせる。main はこの通知まで Offer せず、
            // 旧内容（ブロック無し）の paint を送らない（起動時の不明ID棄却の防止）。
            // 1 セッション 1 回だけ送り、失敗・不在でも XR 駆動は変えない。
            if (stamp !== null && !this._stampNotified) {
              this._stampNotified = true;
              try {
                if (typeof B.stamped === 'function') B.stamped(rec.renderId);
              } catch { /* 通知失敗時は無視 */ }
            }
          } catch (err) {
            console.error('[XRift XR] block overlay failed', err);
          }
        }
        // 最初の数回だけ、record と canvas の対応を記録する（窓側は main が出す）。
        // rAF ポンプで同一 record を繰り返すため、ID が変わったときだけ数える。
        // 毎フレームは出さない（ログ肥大の防止）。
        if (this._recordLogs < 3 && rec.renderId !== this._lastLoggedId) {
          this._lastLoggedId = rec.renderId;
          this._recordLogs++;
          const tagNote = stamp !== null
            ? ` id16=0x${stamp.id16.toString(16).padStart(4, '0')} tag=0x${stamp.tag.toString(16).padStart(4, '0')}`
            : ' stamp=none';
          log(`record #${this._recordLogs} id=${rec.renderId}${tagNote} eye=${eyeW}x${eyeH} canvas=${canvas.width}x${canvas.height}`);
        }
        const st = this._stats;
        const dt = performance.now() - t0;
        st.cb += dt;
        st.maxCb = Math.max(st.maxCb, dt);
        if (dt > 12) st.slow++;
      }
      const st = this._stats;
      st.n++;
      const now = performance.now();
      if (now - st.t > 5000) {
        log(`${((st.n * 1000) / (now - st.t)).toFixed(1)} fps | page render ${(st.cb / st.n).toFixed(1)}ms (max ${st.maxCb.toFixed(1)}, >12ms: ${st.slow}) | record=${rec.renderId} conn=${rec.connEpoch} space=${rec.spaceEpoch}`);
        st.n = 0; st.cb = 0; st.maxCb = 0; st.slow = 0; st.t = now;
      }
    }
    _inputs(hands: HandPose[], frame: XRFrame): void {
      const added: XRInputSource[] = [], removed: XRInputSource[] = [];
      hands.forEach((h, i) => {
        const cur = this._sources[i];
        if (cur && (!h.connected || (h.profile && cur._profile !== h.profile))) { removed.push(cur); this._sources[i] = null; }
        if (h.connected && !this._sources[i]) { const s = new XRInputSource(this, i, h.profile); this._sources[i] = s; added.push(s); }
      });
      if (added.length || removed.length) this._emit('inputsourceschange', new XRInputSourcesChangeEvent('inputsourceschange', { session: this, added, removed }));
      hands.forEach((h, i) => {
        const src = this._sources[i];
        if (!src) return;
        src.gamepad._update(h);
        const p = this._prev[i];
        const t = h.trigger > 0.5, s = h.squeeze > 0.5;
        const fire = (type: string): void => this._emit(type, new XRInputSourceEvent(type, { frame, inputSource: src }));
        if (t && !p.t) fire('selectstart');
        if (!t && p.t) { fire('select'); fire('selectend'); }
        if (s && !p.s) fire('squeezestart');
        if (!s && p.s) { fire('squeeze'); fire('squeezeend'); }
        p.t = t; p.s = s;
      });
    }
    _finish(): void {
      if (this._finished) return;
      this._finished = true;
      this._ended = true;
      this._unsubscribe();
      if (this._overlay !== null) {
        try { this._overlay.remove(); } catch { /* 終了時は無視 */ }
        this._overlay = null;
        this._overlayCtx = null;
      }
      xr._active = null;
      log('session ended');
      // ADR: main の xrActive を落とすため終了を通知する。届かなくても描画は止まるため例外にしない。
      try { if (typeof B.sessionActive === 'function') B.sessionActive(false); } catch { /* 通知失敗時は無視 */ }
      this._emit('end', new XRSessionEvent('end', { session: this }));
    }
  }

  // ------------------------------------------------------------ XRSystem
  const SUPPORTED = new Set(['viewer', 'local', 'local-floor', 'bounded-floor', 'unbounded']);
  // 本番ページが起動直後に呼んでも exe の準備前は false にならないよう、
  // 一定時間だけ到達確認を繰り返す（セッション開始要求の保留と対にする）。
  function sleepMs(ms: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, ms));
  }
  async function waitProbeOk(timeoutMs: number): Promise<boolean> {
    const t0 = Date.now();
    for (;;) {
      try {
        if ((await B.probe()).ok === true) return true;
      } catch {
        // 到達確認の失敗は未準備扱いとし、訴えない（タイムアウト時に 1 行だけ出す）。
      }
      if (Date.now() - t0 >= timeoutMs) return false;
      await sleepMs(500);
    }
  }
  class XRSystem extends EventTarget {
    _active: XRSession | null = null;
    ondevicechange: ((ev: Event) => void) | null = null;
    onsessiongranted: ((ev: Event) => void) | null = null;
    async isSessionSupported(mode: string): Promise<boolean> {
      if (mode === 'inline') return true;
      if (mode !== 'immersive-vr') return false;
      // 本番ページの起動直後の呼び出しに備え、exe の準備ができるまで最大 15 秒待つ。
      // main 側のログで結果を確認できるよう 1 行だけ出す。
      const ok = await waitProbeOk(15000);
      log(`isSessionSupported('immersive-vr') -> ${ok}`);
      return ok;
    }
    async requestSession(mode: string, init: { requiredFeatures?: string[]; optionalFeatures?: string[] } = {}): Promise<XRSession> {
      if (mode !== 'immersive-vr') throw new DOMException(`"${mode}" is not supported`, 'NotSupportedError');
      if (this._active) throw new DOMException('an immersive session is already active', 'InvalidStateError');
      for (const f of init.requiredFeatures || []) {
        if (!SUPPORTED.has(f)) throw new DOMException(`required feature "${f}" is not supported`, 'NotSupportedError');
      }
      const enabled = ['viewer', 'local', ...new Set([...(init.requiredFeatures || []), ...(init.optionalFeatures || [])].filter((f) => SUPPORTED.has(f)))];
      log(`requestSession('${mode}') 要求あり（exe の描画要求待ち。最大 20 秒）`);
      // ADR: main の xrActive を要求時点で立てる。main はこの通知まで record を
      // 転送しないため、解決待ちの前に通知しないと record が来ず deadlock する。
      // 失敗時は落として返す。
      try { if (typeof B.sessionActive === 'function') B.sessionActive(true); } catch { /* 通知失敗時は無視 */ }
      // exe は main 起動時に既に動いているはずであり、ここでは最初の record を待つ。
      // 非装着でも RenderRequest の 1 件目が来れば解決する（準備後の解決）。
      // 1 秒経っても来ない場合は警告だけ出し、待ち自体は最大 20 秒続ける（動作は変えない）。
      let rec: XrRecord;
      const warnTimer = setTimeout(() => {
        log('警告: VR開始から1秒経っても RenderRequest が来ない（exe の pace 停滞を確認）');
      }, 1000);
      try {
        rec = await nextRecord(20000);
      } catch (e) {
        clearTimeout(warnTimer);
        try { if (typeof B.sessionActive === 'function') B.sessionActive(false); } catch { /* 通知失敗時は無視 */ }
        throw e;
      }
      clearTimeout(warnTimer);
      if (!(rec.reqWidth > 0) || !(rec.reqHeight > 0)) {
        try { if (typeof B.sessionActive === 'function') B.sessionActive(false); } catch { /* 通知失敗時は無視 */ }
        throw new DOMException('RenderRequest の描画サイズが不正です', 'NotSupportedError');
      }
      if (!(rec.reqWidth > 0) || !(rec.reqHeight > 0)) throw new DOMException('RenderRequest の描画サイズが不正です', 'NotSupportedError');
      const info: SessionInfo = {
        width: rec.reqWidth, height: rec.reqHeight,
        backend: 'osr', runtimeName: 'xrift_native', systemName: 'XRift OSR',
      };
      log(`OSR: ${info.width}x${info.height} per eye (conn=${rec.connEpoch} space=${rec.spaceEpoch})`);
      const s = new XRSession(info, mode, [...new Set(enabled)]);
      this._active = s;
      return s;
    }
  }
  const xr = new XRSystem();

  // ------------------------------------------------------------ 差し替え（移植元と同一）
  Object.defineProperty(navigator, 'xr', { value: xr, configurable: true, enumerable: true });
  const replace: Record<string, unknown> = {
    XRSystem, XRSession, XRFrame, XRSpace, XRReferenceSpace, XRBoundedReferenceSpace, XRRigidTransform, XRView, XRPose,
    XRViewerPose, XRInputSource, XRWebGLLayer, XRInputSourceEvent, XRInputSourcesChangeEvent, XRSessionEvent,
  };
  for (const [k, v] of Object.entries(replace)) Object.defineProperty(window, k, { value: v, configurable: true, writable: true });
  // three は XRWebGLBinding があると Layers 経路を使うので隠す
  for (const k of ['XRWebGLBinding', 'XRGPUBinding', 'XRProjectionLayer', 'XRMediaBinding']) {
    try { delete (window as unknown as Record<string, unknown>)[k]; } catch { /* 削除できなければ下の上書きで隠す */ }
    try { Object.defineProperty(window, k, { value: undefined, configurable: true, writable: true }); } catch { /* 隠せなければ諦める */ }
  }
  const makeXRCompatible = function (): Promise<void> { return Promise.resolve(); };
  for (const C of [window.WebGLRenderingContext, window.WebGL2RenderingContext]) {
    if (C) (C.prototype as unknown as Record<string, unknown>).makeXRCompatible = makeXRCompatible;
  }
  log('WebXR → OSR bridge installed');
}
