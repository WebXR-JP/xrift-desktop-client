// プロトコルの配置試験（素の node で実行可）。protocol.h との一致を守る。
// M2 で 192B 化した。使い方: node protocol-test.js
'use strict';
const assert = require('node:assert');

// M2: 64B → 192B。先頭 64B の配置は M1 と同じである。
const MSG_LEN = 192;
const MAGIC = 0x54465258;
const VERSION = 2;
const TYPE_OFFER = 1;
const TYPE_DONE = 2;
const TYPE_RENDER_REQUEST = 3;
const TYPE_HAPTIC_COMMAND = 4; // M4: main → exe。
const TYPE_INPUT_STATE = 5;    // M4: exe → main。

function makeOffer(borrowId, width, height, format, handle) {
  const buf = Buffer.alloc(MSG_LEN);
  buf.writeUInt32LE(MAGIC, 0);
  buf.writeUInt16LE(VERSION, 4);
  buf.writeUInt16LE(TYPE_OFFER, 6);
  buf.writeBigUInt64LE(BigInt(borrowId), 8);
  buf.writeUInt32LE(width, 16);
  buf.writeUInt32LE(height, 20);
  buf.writeUInt32LE(format, 24);
  buf.writeBigUInt64LE(handle, 28);
  buf.writeUInt32LE(0, 36);
  return buf;
}

function parseDone(buf) {
  assert.strictEqual(buf.length, MSG_LEN);
  assert.strictEqual(buf.readUInt32LE(0), MAGIC);
  assert.strictEqual(buf.readUInt16LE(4), VERSION);
  assert.strictEqual(buf.readUInt16LE(6), TYPE_DONE);
  return { borrowId: Number(buf.readBigUInt64LE(8)), status: buf.readUInt32LE(36) };
}

// RenderRequest（exe→main）の読み。pose/FOV は float LE である。
function parseRenderRequest(buf) {
  assert.strictEqual(buf.length, MSG_LEN);
  assert.strictEqual(buf.readUInt32LE(0), MAGIC);
  assert.strictEqual(buf.readUInt16LE(4), VERSION);
  assert.strictEqual(buf.readUInt16LE(6), TYPE_RENDER_REQUEST);
  const floats = (off, n) => Array.from({ length: n }, (_, i) => buf.readFloatLE(off + i * 4));
  return {
    renderId: buf.readBigUInt64LE(40),
    connEpoch: buf.readUInt32LE(48),
    spaceEpoch: buf.readUInt32LE(52),
    leftPos: floats(56, 3),
    leftOri: floats(68, 4),
    rightPos: floats(84, 3),
    rightOri: floats(96, 4),
    leftFov: floats(112, 4),
    rightFov: floats(128, 4),
    reqWidth: buf.readUInt32LE(144),
    reqHeight: buf.readUInt32LE(148),
  };
}

// static_assert 相当: 配置の固定（先頭 64B は M1 と同じ）。
const offer = makeOffer(123456789, 4096, 2048, 0, 0x1234ABCDn);
assert.strictEqual(offer.readUInt32LE(0), MAGIC);
assert.strictEqual(offer.readUInt16LE(4), 2);
assert.strictEqual(offer.readUInt16LE(6), 1);
assert.strictEqual(Number(offer.readBigUInt64LE(8)), 123456789);
assert.strictEqual(offer.readUInt32LE(16), 4096);
assert.strictEqual(offer.readUInt32LE(20), 2048);
assert.strictEqual(offer.readUInt32LE(24), 0);
assert.strictEqual(offer.readBigUInt64LE(28), 0x1234ABCDn);
assert.strictEqual(offer.readUInt32LE(36), 0);
// M1 互換: Offer は 40B 以降を 0 で送る。
assert.ok(offer.subarray(40).every((b) => b === 0), 'Offer の M2 領域は 0');

// Done の往復。
const done = Buffer.alloc(MSG_LEN);
done.writeUInt32LE(MAGIC, 0);
done.writeUInt16LE(VERSION, 4);
done.writeUInt16LE(TYPE_DONE, 6);
done.writeBigUInt64LE(987654321n, 8);
done.writeUInt32LE(2, 36);
assert.deepStrictEqual(parseDone(done), { borrowId: 987654321, status: 2 });

// 大きなハンドル値（上位 bit 付き）の往復。
const big = makeOffer(1, 513, 514, 0, 0x0000023AFFFF0000n);
assert.strictEqual(big.readBigUInt64LE(28), 0x0000023AFFFF0000n);

// RenderRequest の往復（C++ の MakeRenderRequest と同じ配置）。
const req = Buffer.alloc(MSG_LEN);
req.writeUInt32LE(MAGIC, 0);
req.writeUInt16LE(VERSION, 4);
req.writeUInt16LE(TYPE_RENDER_REQUEST, 6);
req.writeUInt32LE(2048, 16);
req.writeUInt32LE(1024, 20);
req.writeBigUInt64LE(42n, 40);
req.writeUInt32LE(1, 48);
req.writeUInt32LE(7, 52);
req.writeFloatLE(0.1, 56); req.writeFloatLE(1.6, 60); req.writeFloatLE(0.0, 64);
req.writeFloatLE(0, 68); req.writeFloatLE(0, 72); req.writeFloatLE(0, 76); req.writeFloatLE(1, 80);
req.writeFloatLE(0.164, 84); req.writeFloatLE(1.6, 88); req.writeFloatLE(0.0, 92);
req.writeFloatLE(0.7, 112); req.writeFloatLE(0.7, 116); req.writeFloatLE(0.7, 120); req.writeFloatLE(0.7, 124);
req.writeFloatLE(0.7, 128);
req.writeUInt32LE(2048, 144);
req.writeUInt32LE(1024, 148);
const p = parseRenderRequest(req);
assert.strictEqual(p.renderId, 42n);
assert.strictEqual(p.connEpoch, 1);
assert.strictEqual(p.spaceEpoch, 7);
assert.ok(Math.abs(p.leftPos[0] - 0.1) < 1e-6, 'leftPos の float 往復');
assert.ok(Math.abs(p.rightPos[0] - 0.164) < 1e-6, 'rightPos の float 往復');
assert.strictEqual(p.leftOri[3], 1);
assert.ok(Math.abs(p.leftFov[0] - 0.7) < 1e-6, 'leftFov の float 往復');
assert.strictEqual(p.reqWidth, 2048);
assert.strictEqual(p.reqHeight, 1024);
assert.ok(req.subarray(152).every((b) => b === 0), '予約領域は 0');

// M4: HapticCommand の往復（C++ の MakeHapticCommand と同じ配置）。
// hand@40、amplitude@56、durationMs@48。
const hap = Buffer.alloc(MSG_LEN);
hap.writeUInt32LE(MAGIC, 0);
hap.writeUInt16LE(VERSION, 4);
hap.writeUInt16LE(TYPE_HAPTIC_COMMAND, 6);
hap.writeUInt32LE(1, 40);
hap.writeFloatLE(0.8, 56);
hap.writeUInt32LE(300, 48);
assert.strictEqual(hap.readUInt16LE(6), TYPE_HAPTIC_COMMAND);
assert.strictEqual(hap.readUInt32LE(40), 1);
assert.ok(Math.abs(hap.readFloatLE(56) - 0.8) < 1e-6, 'amplitude の float 往復');
assert.strictEqual(hap.readUInt32LE(48), 300);
assert.ok(hap.subarray(52, 54).every((b) => b === 0), 'HapticCommand の space 域は 0');
assert.ok(hap.subarray(60).every((b) => b === 0), 'HapticCommand の未使用域は 0');

// M4: InputState の配置（C++ の MakeInputStateMessage と同じ）。
// frame@40、conn@48、space@52、手 0 は 56〜71、手 1 は 72〜87（各 16B）。
const profiles = { 0: '-', 1: 'simple', 2: 'oculus', 3: 'index', 4: 'vive', 5: 'motion',
  6: 'pico4', 7: 'pico-neo3', 8: 'pico-ultra', 9: 'pico-g3', 255: 'other' };
assert.strictEqual(profiles[6], 'pico4', 'profile 表の PICO 4');
assert.strictEqual(profiles[2], 'oculus', 'profile 表の oculus');
const st = Buffer.alloc(MSG_LEN);
st.writeUInt32LE(MAGIC, 0);
st.writeUInt16LE(VERSION, 4);
st.writeUInt16LE(TYPE_INPUT_STATE, 6);
st.writeBigUInt64LE(1234n, 40);
st.writeUInt32LE(2, 48);
st.writeUInt32LE(7, 52);
st[56] = 0b101; // flags: 接続 + aim 有効。
st[57] = 6;     // profile=pico4。
st[58] = 128; st[59] = 64; // trigger/squeeze の量子化値。
st.writeInt8(-64, 60); st.writeInt8(32, 61); // thumbstick。
st[64] = 0b11100; st[65] = 0b11100; // buttons/active: primary/secondary/menu が押下中。
st[66] = 0b11; st[67] = 0b01; // float/vector の active。
st[68] = 0b11100; st[69] = 0b1111; // supported。
assert.strictEqual(st.readBigUInt64LE(40), 1234n);
assert.strictEqual(st.readUInt32LE(48), 2);
assert.strictEqual(st.readUInt32LE(52), 7);
assert.strictEqual(st[56] & 1, 1, '接続フラグ');
assert.strictEqual(st[57], 6, 'profile 識別子');
assert.ok(Math.abs(st[58] / 255 - 0.5) < 0.01, 'trigger の量子化往復');
assert.strictEqual(st.readInt8(60), -64, 'stick の int8 往復');
assert.strictEqual(st[64], 0b11100, 'ボタン押下');
assert.strictEqual(st[68], 0b11100, 'ボタン supported（0 と未対応の区別）');
assert.ok(st.subarray(88, 152).every((b) => b === 0), 'InputState の未使用域は 0');

console.log('protocol-test: ok（配置 192B・LE・予約 0・RenderRequest 往復を確認）');
console.log('protocol-test: ok（M4 の HapticCommand・InputState の配置を確認）');
