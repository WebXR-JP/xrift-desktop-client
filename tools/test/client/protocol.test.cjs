// InputState の配置試験（素の node で実行可。前提: `npm run build` で dist が最新）。
// C++ 側（protocol.h の kInput* 配置・--m2-selftest）と同じオフセットで一致を確かめる。
// 使い方: node tools/test/client/protocol.test.cjs
'use strict';
const assert = require('node:assert');
const protocol = require('../../../client/dist/protocol.js');
const { MSG_LEN, MAGIC, VERSION, TYPE_INPUT_STATE, decodeInput } = protocol;

// 固定長と版。C++ の kMessageSize・kVersion と一致すること。
assert.strictEqual(MSG_LEN, 256, 'C++ の kMessageSize と一致');
assert.strictEqual(VERSION, 3, 'C++ の kVersion と一致');

// 既存欄のオフセットは変えない（RenderRequest と共通）。
const GRIP_L = 88, AIM_L = 116, GRIP_R = 160, AIM_R = 188;
assert.strictEqual(AIM_L, GRIP_L + 28, '左 aim は左 grip の直後');
assert.strictEqual(AIM_R, GRIP_R + 28, '右 aim は右 grip の直後');
assert.ok(AIM_R + 28 <= MSG_LEN, 'pose 領域はメッセージに収まる');

// 合成 InputState の復号。左は grip/aim とも有効、右は aim のみ有効。
const m = Buffer.alloc(MSG_LEN);
m.writeUInt32LE(MAGIC, 0);
m.writeUInt16LE(VERSION, 4);
m.writeUInt16LE(TYPE_INPUT_STATE, 6);
m.writeBigUInt64LE(78n, 40);
m.writeUInt32LE(2, 48);
m.writeUInt32LE(7, 52);
m[56] = 0b111; // 左: 接続＋grip 有効＋aim 有効。
m[57] = 6;     // profile=pico4。
m[72] = 0b101; // 右: 接続＋aim 有効（grip 無効）。
m.writeFloatLE(0.11, GRIP_L); m.writeFloatLE(1.21, GRIP_L + 4); m.writeFloatLE(-0.31, GRIP_L + 8);
m.writeFloatLE(0, GRIP_L + 12); m.writeFloatLE(0, GRIP_L + 16);
m.writeFloatLE(0, GRIP_L + 20); m.writeFloatLE(1, GRIP_L + 24);
m.writeFloatLE(0.12, AIM_L); m.writeFloatLE(0, AIM_L + 16 + 4);
m.writeFloatLE(-0.13, AIM_R); m.writeFloatLE(1, AIM_R + 24);
m[152] = 0b100; m[153] = 0b1000;

const snap = decodeInput(m);
assert.ok(snap !== null, '合成 InputState を復号できる');
assert.strictEqual(snap.frame, '78');
assert.strictEqual(snap.connEpoch, 2);
assert.strictEqual(snap.spaceEpoch, 7);
// 有効な pose は値が読める。
assert.strictEqual(snap.left.connected, true);
assert.strictEqual(snap.left.grip.valid, true);
assert.ok(Math.abs(snap.left.grip.position[0] - 0.11) < 1e-6, '左 grip 位置の往復');
assert.strictEqual(snap.left.grip.orientation[3], 1, '左 grip 姿勢の往復');
assert.strictEqual(snap.left.aim.valid, true);
assert.ok(Math.abs(snap.left.aim.position[0] - 0.12) < 1e-6, '左 aim 位置の往復');
assert.strictEqual(snap.right.aim.valid, true);
assert.ok(Math.abs(snap.right.aim.position[0] + 0.13) < 1e-6, '右 aim 位置の往復');
// 無効な右 grip は valid=false で 0 埋め（シムは null を返す）。
assert.strictEqual(snap.right.grip.valid, false, '無効な右 grip は valid=false');
assert.ok(snap.right.grip.position.every((v) => v === 0), '無効な右 grip 位置は 0');
assert.ok(snap.right.grip.orientation.every((v) => v === 0), '無効な右 grip 姿勢は 0');
// ラッチ・ボタン欄は従来どおり。
assert.strictEqual(snap.latchLeft, 'primary');
assert.strictEqual(snap.latchRight, 'secondary');

// 未接続の両手は flags=0 で pose も無効（0 埋めと区別するため flags を見る）。
const empty = Buffer.alloc(MSG_LEN);
empty.writeUInt32LE(MAGIC, 0);
empty.writeUInt16LE(VERSION, 4);
empty.writeUInt16LE(TYPE_INPUT_STATE, 6);
const emptySnap = decodeInput(empty);
assert.ok(emptySnap !== null, '空 InputState を復号できる');
assert.strictEqual(emptySnap.left.connected, false);
assert.strictEqual(emptySnap.left.grip.valid, false, '未接続の左 grip は無効');
assert.strictEqual(emptySnap.left.aim.valid, false, '未接続の左 aim は無効');
assert.strictEqual(emptySnap.right.grip.valid, false, '未接続の右 grip は無効');

// 短いメッセージは拒否する。
assert.strictEqual(decodeInput(Buffer.alloc(192)), null, '192B は旧枠のため拒否');

console.log('protocol.test: ok（配置 256B・既存オフセット不変・pose 往復・無効時 null 相当を確認）');
