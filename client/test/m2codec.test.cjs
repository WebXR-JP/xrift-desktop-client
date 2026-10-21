// M2 コーデック試験（素の node で実行可。前提: `npm run build` で dist が最新）。
// C++ 側（render_record.h の --m2-selftest・harness の m2-codec-test.js）と同じ
// ベクトルで一致を確かめる。期待値を変えたら両側を同時に更新すること。
// 使い方: node test/m2codec.test.cjs
'use strict';
const assert = require('node:assert');
const codec = require('../dist/m2codec.js');
const { quantizePos, quantizeOri, poseTag, renderId16, encodeBlock16, decodeBlock16 } = codec;

// 量子化の両端と範囲外の寄せ（C++ の --m2-selftest と同じ条件）。
assert.strictEqual(quantizePos(-4.0), 0);
assert.strictEqual(quantizePos(4.0), 255);
assert.strictEqual(quantizePos(-99), 0);
assert.strictEqual(quantizePos(99), 255);
assert.strictEqual(quantizeOri(-1.0), 0);
assert.strictEqual(quantizeOri(1.0), 255);

// C++ の --m2-selftest・harness の m2-codec-test.js が出すタグベクトルとの一致。
const left = { position: [0.1, 1.6, 0.0], orientation: [0, 0, 0, 1] };
const right = { position: [0.164, 1.6, 0.0], orientation: [0, 0, 0, 1] };
const tagA = poseTag(42n, 1, 7, left, right);
assert.strictEqual(tagA, 0x7c39, `C++ とのタグ一致 (id=42): 0x${tagA.toString(16)}`);
const leftMoved = { position: [0.2, 1.6, 0.0], orientation: [0, 0, 0, 1] };
assert.notStrictEqual(poseTag(42n, 1, 7, leftMoved, right), tagA, 'pose 変化でタグが変わる');
assert.notStrictEqual(poseTag(43n, 1, 7, left, right), tagA, 'ID 変化でタグが変わる');
assert.notStrictEqual(poseTag(42n, 1, 8, left, right), tagA, '世代変化でタグが変わる');
const tagB = poseTag(0x123456789ABCDEF0n, 3, 11, leftMoved, left);
assert.strictEqual(tagB, 0xc36a, `C++ とのタグ一致 (id=0x1234..): 0x${tagB.toString(16)}`);

// パイプは renderId を 10 進文字列で運ぶ。文字列でも bigint と同じ結果になること。
assert.strictEqual(poseTag('42', 1, 7, left, right), tagA, '文字列 ID でもタグ一致');
assert.strictEqual(poseTag('1311768467463790320', 3, 11, leftMoved, left), tagB, '大きな文字列 ID でもタグ一致');
assert.strictEqual(renderId16('42'), 42);
assert.strictEqual(renderId16('1311768467463790320'), 0xdef0);

// ブロック符号化の往復（全 16bit 値の代表点）。
for (const v of [0, 1, 0x7c39, 0xc36a, 0xffff, 0x8000, 0x00ff]) {
  assert.strictEqual(decodeBlock16(encodeBlock16(v)), v, `ブロック往復 0x${v.toString(16)}`);
}

// 混入の検出（段階 2 の核心）。新 ID・旧 pose の組はタグ不一致になること。
const freshTag = poseTag(100n, 1, 7, leftMoved, right);
const mixedTag = poseTag(100n, 1, 7, left, right);
assert.notStrictEqual(freshTag, mixedTag, '新 ID に旧 pose を混ぜるとタグ不一致');

console.log(`m2codec.test: ok（量子化・タグ一致 id=42:0x${tagA.toString(16)} ` +
  `id=0x1234..:0x${tagB.toString(16)}・文字列 ID・ブロック往復・混入検出を確認）`);
