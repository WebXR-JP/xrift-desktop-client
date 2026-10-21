// M2 コーデックの共有定義。C++ 側（render_record.h・m2_blocks.h）と一致させること。
//  - 位置は ±4m を 8bit に、姿勢成分は ±1 を 8bit に量子化する
//  - pose タグは量子化 pose + 世代 + renderId 全体の FNV-1a 下位 16bit
//  - ブロックは 32x32・4x4 セル・16bit（1=白/0=黒。復号はセル中心画素）
// node からは require で、ブラウザからは <script> で window.__m2codec として使う。
// 使い方（node）: const codec = require('./m2-codec.js');
'use strict';
(function (root) {
  const POS_MIN = -4.0, POS_MAX = 4.0;
  const ORI_MIN = -1.0, ORI_MAX = 1.0;

  function quantizeByte(v, lo, hi) {
    const t = (v - lo) / (hi - lo) * 255;
    if (t < 0) return 0;
    if (t > 255) return 255;
    return Math.floor(t + 0.5);
  }
  const quantizePos = (v) => quantizeByte(v, POS_MIN, POS_MAX);
  const quantizeOri = (v) => quantizeByte(v, ORI_MIN, ORI_MAX);

  // C++ の PoseTag と同じバイト列（計 30B）で計算する。
  // renderId LE 8B、connEpoch LE 4B、spaceEpoch LE 4B、
  // 左 pos 3B、左 ori 4B、右 pos 3B、右 ori 4B。
  function poseTag(renderId, connEpoch, spaceEpoch, left, right) {
    const bytes = [];
    let id = BigInt(renderId);
    for (let i = 0; i < 8; i++) {
      bytes.push(Number(id & 0xFFn));
      id >>= 8n;
    }
    for (let i = 0; i < 4; i++) bytes.push((connEpoch >>> (i * 8)) & 0xFF);
    for (let i = 0; i < 4; i++) bytes.push((spaceEpoch >>> (i * 8)) & 0xFF);
    for (let i = 0; i < 3; i++) bytes.push(quantizePos(left.position[i]));
    for (let i = 0; i < 4; i++) bytes.push(quantizeOri(left.orientation[i]));
    for (let i = 0; i < 3; i++) bytes.push(quantizePos(right.position[i]));
    for (let i = 0; i < 4; i++) bytes.push(quantizeOri(right.orientation[i]));
    let hash = 2166136261 >>> 0;
    for (const b of bytes) {
      hash ^= b;
      hash = Math.imul(hash, 16777619) >>> 0;
    }
    return hash & 0xFFFF;
  }

  function encodeBlock16(value) {
    const px = new Uint8Array(32 * 32 * 4);
    for (let b = 0; b < 16; b++) {
      const on = ((value >>> b) & 1) === 1;
      const x0 = (b % 8) * 4, y0 = Math.floor(b / 8) * 4;
      for (let dy = 0; dy < 4; dy++) {
        for (let dx = 0; dx < 4; dx++) {
          const o = (((y0 + dy) * 32 + (x0 + dx)) * 4);
          const c = on ? 255 : 0;
          px[o] = c; px[o + 1] = c; px[o + 2] = c; px[o + 3] = 255;
        }
      }
    }
    return px;
  }

  function decodeBlock16(px) {
    let v = 0;
    for (let b = 0; b < 16; b++) {
      const cx = (b % 8) * 4 + 2, cy = Math.floor(b / 8) * 4 + 2;
      const o = ((cy * 32 + cx) * 4);
      const avg = (px[o] + px[o + 1] + px[o + 2]) / 3;
      if (avg > 128) v |= (1 << b);
    }
    return v >>> 0;
  }

  const api = { quantizeByte, quantizePos, quantizeOri, poseTag, encodeBlock16, decodeBlock16 };
  if (typeof module !== 'undefined' && module.exports) {
    module.exports = api;
  } else {
    root.__m2codec = api;
  }
})(typeof window !== 'undefined' ? window : globalThis);
