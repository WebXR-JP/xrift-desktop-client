// capturePage PNG から M2 制御ブロックを復号する検証スクリプト。
// PNG は表示方向（左上原点）であり、overlay が描いた座標をそのまま読む。
// 使い方: node app/scripts/decode-m5-png.cjs <png> [--expect-id16=0x1234] [--expect-tag=0x5678]
'use strict';
const assert = require('node:assert');
const { loadImage, createCanvas } = require('@napi-rs/canvas');

function decodeBlock16At(data, strideW, ox, oy) {
  let v = 0;
  for (let b = 0; b < 16; b++) {
    const cx = ox + (b % 8) * 4 + 2;
    const cy = oy + Math.floor(b / 8) * 4 + 2;
    const o = (cy * strideW + cx) * 4;
    const avg = (data[o] + data[o + 1] + data[o + 2]) / 3;
    if (avg > 128) v |= (1 << b);
  }
  return v >>> 0;
}

function px(data, strideW, x, y) {
  const o = (y * strideW + x) * 4;
  return [data[o], data[o + 1], data[o + 2], data[o + 3]];
}

const isMagenta = (p) => p[0] > 200 && p[2] > 200 && p[1] < 80;
const isCyan = (p) => p[1] > 200 && p[2] > 200 && p[0] < 80;

async function main() {
  const args = process.argv.slice(2);
  const png = args.find((a) => !a.startsWith('--'));
  if (!png) throw new Error('使い方: decode-m5-png.cjs <png> [--expect-id16=..] [--expect-tag=..]');
  const opt = (name) => {
    const a = args.find((x) => x.startsWith(`--${name}=`));
    return a === undefined ? null : parseInt(a.slice(name.length + 3), 16);
  };
  const expectId16 = opt('expect-id16');
  const expectTag = opt('expect-tag');

  const img = await loadImage(png);
  const cv = createCanvas(img.width, img.height);
  const ctx = cv.getContext('2d');
  ctx.drawImage(img, 0, 0);
  const { data } = ctx.getImageData(0, 0, img.width, img.height);
  const W = img.width, H = img.height;
  const halfW = Math.floor(W / 2);

  const id16 = decodeBlock16At(data, W, 64, 64);
  const tag = decodeBlock16At(data, W, 128, 64);
  const mag = px(data, W, 160 + 16, 8 + 16);
  const cyn = px(data, W, halfW + 160 + 16, 8 + 16);
  const out = {
    png, size: `${W}x${H}`, halfW,
    blockA: `0x${id16.toString(16).padStart(4, '0')}`,
    blockB: `0x${tag.toString(16).padStart(4, '0')}`,
    markerLeft: `[${mag.join(',')}] magenta=${isMagenta(mag)}`,
    markerRight: `[${cyn.join(',')}] cyan=${isCyan(cyn)}`,
  };
  console.log(JSON.stringify(out));

  assert.ok(W >= 192 && H >= 96, `画像が小さすぎてブロック配置が無い: ${W}x${H}`);
  if (expectId16 !== null) assert.strictEqual(id16, expectId16, 'ブロック A 不一致');
  if (expectTag !== null) assert.strictEqual(tag, expectTag, 'ブロック B 不一致');
  assert.ok(isMagenta(mag), `左マーカーがマゼンタでない: [${mag.join(',')}]`);
  assert.ok(isCyan(cyn), `右マーカーがシアンでない: [${cyn.join(',')}]`);
  console.log('decode-m5-png: ok（ブロック A/B・マーカー色を確認）');
}

main().catch((e) => { console.error('decode-m5-png FAILED:', e instanceof Error ? e.message : e); process.exit(1); });
