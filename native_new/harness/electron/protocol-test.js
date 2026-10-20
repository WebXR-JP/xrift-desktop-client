// プロトコルの配置試験（素の node で実行可）。protocol.h との一致を守る。
// 使い方: node protocol-test.js
'use strict';
const assert = require('node:assert');

const MSG_LEN = 64;
const MAGIC = 0x54465258;
const VERSION = 1;

function makeOffer(borrowId, width, height, format, handle) {
  const buf = Buffer.alloc(MSG_LEN);
  buf.writeUInt32LE(MAGIC, 0);
  buf.writeUInt16LE(VERSION, 4);
  buf.writeUInt16LE(1, 6);
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
  assert.strictEqual(buf.readUInt16LE(6), 2);
  return { borrowId: Number(buf.readBigUInt64LE(8)), status: buf.readUInt32LE(36) };
}

// static_assert 相当: 配置の固定。
const offer = makeOffer(123456789, 4096, 2048, 0, 0x1234ABCDn);
assert.strictEqual(offer.readUInt32LE(0), MAGIC);
assert.strictEqual(offer.readUInt16LE(4), 1);
assert.strictEqual(offer.readUInt16LE(6), 1);
assert.strictEqual(Number(offer.readBigUInt64LE(8)), 123456789);
assert.strictEqual(offer.readUInt32LE(16), 4096);
assert.strictEqual(offer.readUInt32LE(20), 2048);
assert.strictEqual(offer.readUInt32LE(24), 0);
assert.strictEqual(offer.readBigUInt64LE(28), 0x1234ABCDn);
assert.strictEqual(offer.readUInt32LE(36), 0);
assert.ok(offer.subarray(40).every((b) => b === 0), '予約領域は 0');

// Done の往復。
const done = Buffer.alloc(MSG_LEN);
done.writeUInt32LE(MAGIC, 0);
done.writeUInt16LE(VERSION, 4);
done.writeUInt16LE(2, 6);
done.writeBigUInt64LE(987654321n, 8);
done.writeUInt32LE(2, 36);
assert.deepStrictEqual(parseDone(done), { borrowId: 987654321, status: 2 });

// 大きなハンドル値（上位 bit 付き）の往復。
const big = makeOffer(1, 513, 514, 0, 0x0000023AFFFF0000n);
assert.strictEqual(big.readBigUInt64LE(28), 0x0000023AFFFF0000n);

console.log('protocol-test: ok（配置 64B・LE・予約 0 を確認）');
