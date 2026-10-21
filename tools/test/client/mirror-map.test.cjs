// レターボックス往復の試験（素の node で実行可。前提: client/pages/mirror-map.js が正本）。
// コントロール canvas と OSR content の座標対応が崩れると入力転送の位置がずれるため、
// 境界（黒帯・アスペクト差・縮退入力）をここで押さえる。
// 使い方: node tools/test/client/mirror-map.test.cjs
'use strict';
const assert = require('node:assert');
const map = require('../../../client/pages/mirror-map.js');

// 同一アスペクトでは全面に広がり、黒帯が出ない。
{
  const lb = map.computeLetterbox(1280, 800, 960, 600);
  assert.ok(lb !== null, '同一アスペクトで矩形が求まる');
  assert.ok(Math.abs(lb.scale - 0.75) < 1e-9, '倍率は min 側で一致');
  assert.ok(Math.abs(lb.dw - 960) < 1e-9 && Math.abs(lb.dh - 600) < 1e-9, '全面に広がる');
  assert.ok(Math.abs(lb.ox) < 1e-9 && Math.abs(lb.oy) < 1e-9, '黒帯は無い');
}

// OSR が横長で canvas が縦長の場合、上下に黒帯が出る。
{
  const lb = map.computeLetterbox(1280, 800, 400, 800);
  assert.ok(lb !== null, '縦長 canvas でも矩形が求まる');
  assert.ok(Math.abs(lb.scale - 0.3125) < 1e-9, '幅基準の倍率');
  assert.ok(Math.abs(lb.dw - 400) < 1e-9, '幅いっぱい');
  assert.ok(lb.oy > 0, '上下に黒帯');
  const top = map.canvasToOsr(200, lb.oy - 1, 1280, 800, 400, 800);
  assert.strictEqual(top, null, '上黒帯の点は捨てる');
  const mid = map.canvasToOsr(200, 400, 1280, 800, 400, 800);
  assert.ok(mid !== null, '表示域の中央は OSR 座標へ戻る');
  assert.ok(Math.abs(mid.x - 640) < 1, '中央の x は OSR 中央');
}

// VR サイズ（横長・2:1）に 4:3 よりの窓を合わせると上下に黒帯が出る。
{
  const lb = map.computeLetterbox(1280, 640, 960, 600);
  assert.ok(lb !== null, 'VR 比でも矩形が求まる');
  assert.ok(lb.oy > 0, '上下に黒帯');
  const bar = map.canvasToOsr(480, lb.oy - 1, 1280, 640, 960, 600);
  assert.strictEqual(bar, null, '上黒帯の点は捨てる');
  const mid = map.canvasToOsr(480, 300, 1280, 640, 960, 600);
  assert.ok(mid !== null, '表示域の中央は OSR 座標へ戻る');
  assert.ok(Math.abs(mid.x - 640) < 2 && Math.abs(mid.y - 320) < 2, '中央は OSR 中央');
}

// 隅の対応。右下の内側は OSR 右下に寄り、外側は捨てる。
{
  const lb = map.computeLetterbox(1280, 800, 960, 600);
  assert.ok(lb !== null, '前提の矩形');
  const inner = map.canvasToOsr(959, 599, 1280, 800, 960, 600);
  assert.ok(inner !== null, '右下の内側は拾う');
  assert.ok(inner.x > 1270 && inner.y > 790, '右下に寄る');
  assert.strictEqual(map.canvasToOsr(960, 300, 1280, 800, 960, 600), null, '右端の外は捨てる');
  assert.strictEqual(map.canvasToOsr(-1, 300, 1280, 800, 960, 600), null, '左端の外は捨てる');
}

// 縮退した入力は全て捨てる（誤って全面クリック扱いにしない）。
assert.strictEqual(map.computeLetterbox(0, 800, 960, 600), null, 'OSR 幅 0 は捨てる');
assert.strictEqual(map.computeLetterbox(1280, 800, 0, 600), null, 'canvas 幅 0 は捨てる');
assert.strictEqual(map.computeLetterbox(NaN, 800, 960, 600), null, 'NaN は捨てる');
assert.strictEqual(map.canvasToOsr(100, 100, 0, 800, 960, 600), null, 'OSR 幅 0 の変換は捨てる');
assert.strictEqual(map.canvasToOsr(NaN, 100, 1280, 800, 960, 600), null, 'NaN 座標は捨てる');

console.log('mirror-map.test.cjs: 全件通過');
