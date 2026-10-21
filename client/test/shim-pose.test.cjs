// シムの pose 検証（素の node で実行可。前提: `npm run build` で dist が最新）。
// ブラウザの最小スタブ上で `installM5XR` を動かし、以下を確かめる。
//   - pose 無効時（非装着相当）: `targetRaySpace` / `gripSpace` の `getPose` が
//     null を返し、例外が出ない
//   - pose 有効時: `getPose` が position/orientation 由来の行列を返す
// 使い方: node test/shim-pose.test.cjs
'use strict';
const assert = require('node:assert');

function makeWindow() {
  const rafQueue = [];
  const win = {
    requestAnimationFrame(cb) {
      rafQueue.push(cb);
      return rafQueue.length;
    },
  };
  return { win, rafQueue };
}

function makeDocument() {
  const ctx2d = {
    fillStyle: '',
    fillRect() {},
  };
  const el = {
    width: 0,
    height: 0,
    style: {},
    setAttribute() {},
    getContext() { return ctx2d; },
    getBoundingClientRect() { return { left: 0, top: 0, width: 2, height: 1 }; },
  };
  return {
    createElement() { return el; },
    documentElement: { appendChild() {} },
  };
}

function makeBridge() {
  const recordCbs = [];
  const inputCbs = [];
  return {
    recordCbs,
    inputCbs,
    haptics: [],
    onRecord(cb) { recordCbs.push(cb); },
    onInput(cb) { inputCbs.push(cb); },
    haptic(hand, intensity, durationMs) { this.haptics.push({ hand, intensity, durationMs }); },
    async probe() { return { ok: true }; },
  };
}

const INVALID_POSE = { valid: false, position: [0, 0, 0], orientation: [0, 0, 0, 0] };
function handWire({ connected, profile = '', grip = INVALID_POSE, aim = INVALID_POSE }) {
  return {
    connected, profile, grip, aim,
    trigger: 0, squeeze: 0,
    thumbstickClick: false, trackpadClick: false, primary: false, secondary: false, menu: false,
    thumbstick: [0, 0], trackpad: [0, 0], hasThumbstick: false, hasTrackpad: false,
  };
}

const RECORD = {
  renderId: '7', connEpoch: 1, spaceEpoch: 1,
  leftPos: [0, 1.6, 0], leftOri: [0, 0, 0, 1],
  rightPos: [0.064, 1.6, 0], rightOri: [0, 0, 0, 1],
  leftFov: [0.7, 0.7, 0.7, 0.7], rightFov: [0.7, 0.7, 0.7, 0.7],
  reqWidth: 512, reqHeight: 512, staleHint: false,
};

async function main() {
  const { win } = makeWindow();
  const bridge = makeBridge();
  win.__xriftXR = bridge;
  globalThis.window = win;
  // Node 24 の `navigator` は getter のため上書き定義する。
  Object.defineProperty(globalThis, 'navigator', { value: {}, configurable: true, writable: true });
  globalThis.document = makeDocument();

  const { installM5XR } = require('../dist/shim.js');
  installM5XR();
  const xr = globalThis.navigator.xr;
  assert.ok(xr, 'navigator.xr が差し替わる');
  assert.strictEqual(await xr.isSessionSupported('immersive-vr'), true);

  // 非装着相当: 左は接続するが pose は無効、右は未接続。
  bridge.inputCbs.forEach((cb) => cb({
    left: handWire({ connected: true, profile: 'generic-trigger-squeeze-thumbstick' }),
    right: handWire({ connected: false }),
    latchLeft: '-', latchRight: '-',
  }));
  bridge.recordCbs.forEach((cb) => cb(RECORD));

  const session = await xr.requestSession('immersive-vr', {});
  const fakeCanvas = {
    width: 0, height: 0, style: {},
    getBoundingClientRect() { return { left: 0, top: 0, width: 2, height: 1 }; },
  };
  const fakeGl = { getContextAttributes: () => ({ antialias: false }), canvas: fakeCanvas };
  const layer = new win.XRWebGLLayer(session, fakeGl);
  session.updateRenderState({ baseLayer: layer });
  // 基準空間は unbounded（恒等写像）を使い、入力 pose そのままを確かめる。
  const refSpace = await session.requestReferenceSpace('unbounded');

  // pose 無効時は getPose が null を返し、例外が出ない。
  let frame = null;
  session.requestAnimationFrame((_t, f) => { frame = f; });
  assert.ok(frame !== null, '初回 record でフレームが進む');
  assert.strictEqual(session.inputSources.length, 1, '左だけが inputSources に出る');
  assert.strictEqual(session.inputSources[0].handedness, 'left');
  const src = session.inputSources[0];
  assert.strictEqual(frame.getPose(src.gripSpace, refSpace), null, '無効な grip は null');
  assert.strictEqual(frame.getPose(src.targetRaySpace, refSpace), null, '無効な aim は null');

  // 有効な pose を流すと行列が返る（position/orientation の変換）。
  bridge.inputCbs.forEach((cb) => cb({
    left: handWire({
      connected: true,
      grip: { valid: true, position: [0.11, 1.21, -0.31], orientation: [0, 0, 0, 1] },
      aim: { valid: true, position: [0.12, 1.22, -0.32], orientation: [0, 0, 0, 1] },
    }),
    right: handWire({ connected: false }),
    latchLeft: '-', latchRight: '-',
  }));
  let frame2 = null;
  session.requestAnimationFrame((_t, f) => { frame2 = f; });
  bridge.recordCbs.forEach((cb) => cb({ ...RECORD, renderId: '8' }));
  assert.ok(frame2 !== null, '2 件目の record でフレームが進む');
  const gripPose = frame2.getPose(src.gripSpace, refSpace);
  assert.ok(gripPose !== null, '有効な grip は pose を返す');
  assert.ok(Math.abs(gripPose.transform.position.x - 0.11) < 1e-6, 'grip 位置 x が一致');
  assert.ok(Math.abs(gripPose.transform.position.y - 1.21) < 1e-6, 'grip 位置 y が一致');
  assert.ok(Math.abs(gripPose.transform.position.z + 0.31) < 1e-6, 'grip 位置 z が一致');
  assert.strictEqual(gripPose.transform.orientation.w, 1, 'grip 姿勢 w が一致');
  const aimPose = frame2.getPose(src.targetRaySpace, refSpace);
  assert.ok(aimPose !== null, '有効な aim は pose を返す');
  assert.ok(Math.abs(aimPose.transform.position.x - 0.12) < 1e-6, 'aim 位置 x が一致');

  // 不正な値（非有限・ゼロ回転）が来ても例外にせず null に倒す。
  bridge.inputCbs.forEach((cb) => cb({
    left: handWire({
      connected: true,
      grip: { valid: true, position: [NaN, 0, 0], orientation: [0, 0, 0, 1] },
      aim: { valid: true, position: [0, 0, 0], orientation: [0, 0, 0, 0] },
    }),
    right: handWire({ connected: false }),
    latchLeft: '-', latchRight: '-',
  }));
  let frame3 = null;
  session.requestAnimationFrame((_t, f) => { frame3 = f; });
  bridge.recordCbs.forEach((cb) => cb({ ...RECORD, renderId: '9' }));
  assert.ok(frame3 !== null, '3 件目の record でフレームが進む');
  assert.strictEqual(frame3.getPose(src.gripSpace, refSpace), null, '非有限の grip は null');
  assert.strictEqual(frame3.getPose(src.targetRaySpace, refSpace), null, 'ゼロ回転の aim は null');

  await session.end();
  console.log('shim-pose.test: ok（無効時 null・例外なし・有効時行列・不正値 null を確認）');
}

main().catch((e) => {
  console.error(e);
  process.exitCode = 1;
});
