// 入力転送の境界試験（素の node で実行可。前提: `npm run build` で dist が最新）。
// renderer からの IPC 値（unknown）を OSR の sendInputEvent 形へ直す変換が、
// マウス・ホイール・キーの全種別で崩れないことと、不正値を捨てることを確かめる。
// 使い方: node tools/test/client/mirror-input.test.cjs
'use strict';
const assert = require('node:assert');
const { toOsrInputEvent } = require('../../../client/dist/mirror-input.js');

const W = 1280, H = 800;

// マウス移動は OSR 面内へ丸めて送る。
{
  const ev = toOsrInputEvent({ kind: 'mouseMove', x: 100.6, y: 200.4 }, W, H);
  assert.ok(ev !== null && ev.type === 'mouseMove', '移動を送れる');
  assert.strictEqual(ev.x, 101, 'x は丸める');
  assert.strictEqual(ev.y, 200, 'y は丸める');
}

// クリックはボタンと回数を保つ。
{
  const ev = toOsrInputEvent({ kind: 'mouseDown', x: 640, y: 400, button: 'left', clickCount: 2 }, W, H);
  assert.ok(ev !== null && ev.type === 'mouseDown', '押下を送れる');
  assert.strictEqual(ev.button, 'left', 'ボタン維持');
  assert.strictEqual(ev.clickCount, 2, 'ダブルクリック維持');
  const up = toOsrInputEvent({ kind: 'mouseUp', x: 640, y: 400, button: 'right', clickCount: 1 }, W, H);
  assert.ok(up !== null && up.type === 'mouseUp' && up.button === 'right', '右ボタンの離しを送れる');
}

// ホイールは縦横の量を保つ。
{
  const ev = toOsrInputEvent({ kind: 'mouseWheel', x: 640, y: 400, deltaX: 0, deltaY: -120 }, W, H);
  assert.ok(ev !== null && ev.type === 'mouseWheel', 'ホイールを送れる');
  assert.strictEqual(ev.deltaY, -120, '縦量維持');
  assert.strictEqual(ev.deltaX, 0, '横量維持');
}

// キーは keyDown/keyUp/char の 3 種を送れる（テキスト入力は char）。
{
  const down = toOsrInputEvent({ kind: 'keyDown', keyCode: 'a', modifiers: ['shift'] }, W, H);
  assert.ok(down !== null && down.type === 'keyDown', 'keyDown を送れる');
  assert.strictEqual(down.keyCode, 'a', '符号維持');
  assert.deepStrictEqual(down.modifiers, ['shift'], '修飾子維持');
  const up = toOsrInputEvent({ kind: 'keyUp', keyCode: 'Enter', modifiers: [] }, W, H);
  assert.ok(up !== null && up.type === 'keyUp' && up.keyCode === 'Enter', 'Enter の keyUp を送れる');
  const ch = toOsrInputEvent({ kind: 'char', keyCode: 'あ' }, W, H);
  assert.ok(ch !== null && ch.type === 'char' && ch.keyCode === 'あ', 'char で日本語を送れる');
}

// 範囲外の座標は面内へ寄せる（黒帯の丸め誤差で落とさない）。
{
  const ev = toOsrInputEvent({ kind: 'mouseMove', x: 5000, y: -30 }, W, H);
  assert.ok(ev !== null, '範囲外でも捨てない');
  assert.strictEqual(ev.x, W - 1, '右端へ寄せる');
  assert.strictEqual(ev.y, 0, '上端へ寄せる');
}

// 不正値は捨てる（OSR へ誤入力を送らない）。
assert.strictEqual(toOsrInputEvent(null, W, H), null, 'null は捨てる');
assert.strictEqual(toOsrInputEvent({ kind: 'mouseMove', x: NaN, y: 1 }, W, H), null, 'NaN 座標は捨てる');
assert.strictEqual(toOsrInputEvent({ kind: 'mouseDown', x: 1, y: 1, button: 'side', clickCount: 1 }, W, H), null, '未知ボタンは捨てる');
assert.strictEqual(toOsrInputEvent({ kind: 'keyDown', keyCode: '', modifiers: [] }, W, H), null, '空符号は捨てる');
// キーに座標は無いため OSR 寸法には依存しない。マウス系だけ寸法を要する。
assert.strictEqual(toOsrInputEvent({ kind: 'mouseMove', x: 1, y: 1 }, 0, H), null, 'OSR 幅 0 のマウスは捨てる');
assert.strictEqual(toOsrInputEvent({ kind: 'unknown-kind' }, W, H), null, '未知種別は捨てる');

// 未知の修飾子は落とし、既知だけ残す。
{
  const ev = toOsrInputEvent({ kind: 'keyDown', keyCode: 'a', modifiers: ['shift', 'hogekey'] }, W, H);
  assert.ok(ev !== null && ev.type === 'keyDown', '既知があれば送れる');
  assert.deepStrictEqual(ev.modifiers, ['shift'], '未知修飾子は落とす');
}

console.log('mirror-input.test.cjs: 全件通過');
