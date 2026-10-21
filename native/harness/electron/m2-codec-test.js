// M2 コーデック試験（素の node で実行可）。C++ 側（render_record.h）と一致させること。
// 量子化・pose タグ・ブロック符号化の両側同一性を確かめる。
// 使い方: node m2-codec-test.js
'use strict';
const assert = require('node:assert');
const codec = require('./m2-codec.js');
const { quantizePos, quantizeOri, poseTag, encodeBlock16, decodeBlock16 } = codec;

// 量子化の両端と範囲外の寄せ（C++ の --m2-selftest と同じ条件）。
assert.strictEqual(quantizePos(-4.0), 0);
assert.strictEqual(quantizePos(4.0), 255);
assert.strictEqual(quantizePos(-99), 0);
assert.strictEqual(quantizePos(99), 255);
assert.strictEqual(quantizeOri(-1.0), 0);
assert.strictEqual(quantizeOri(1.0), 255);

// C++ の --m2-selftest が出すタグベクトルとの一致。render_record.h を変えたら
// 両側の期待値を同時に更新する。
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

// ブロック符号化の往復（全 16bit 値の代表点）。
for (const v of [0, 1, 0x7c39, 0xc36a, 0xFFFF, 0x8000, 0x00FF]) {
  assert.strictEqual(decodeBlock16(encodeBlock16(v)), v, `ブロック往復 0x${v.toString(16)}`);
}

// 混入の検出（段階 2 の核心）。新 ID・旧 pose の組はタグ不一致になること。
const freshTag = poseTag(100n, 1, 7, leftMoved, right);
const mixedTag = poseTag(100n, 1, 7, left, right);
assert.notStrictEqual(freshTag, mixedTag, '新 ID に旧 pose を混ぜるとタグ不一致');

console.log(`m2-codec-test: ok（量子化・タグ一致 id=42:0x${tagA.toString(16)} ` +
  `id=0x1234..:0x${tagB.toString(16)}・ブロック往復・混入検出を確認）`);
