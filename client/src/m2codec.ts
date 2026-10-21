/**
 * M2 制御ブロックのコーデック（`native/harness/electron/m2-codec.js` の移植）。
 *
 * 正典は C++ 側（`native/src/transfer/render_record.h` の量子化・`PoseTag`、
 * `native/src/transfer/m2_blocks.h` のブロック符号化）であり、このモジュールは
 * その写しである。計算結果を変えたら exe が受理しないため、式を変えるときは
 * C++・harness・ここ・`./shim.ts` 内の同等品を同時に更新し、
 * `../test/m2codec.test.cjs` のベクトルで一致を確認する。
 *
 * @packageDocumentation
 */

/** 制御ブロックの一辺（画素）。 */
export const M2_BLOCK_PIXELS = 32;
/** 制御ブロックの 1 セルの一辺（画素）。 */
export const M2_BLOCK_CELL = 4;
/** ブロック A（renderId 下位 16bit）の左上 x（アトラス座標）。 */
export const M2_BLOCK_ORG_AX = 64;
/** ブロック B（pose タグ 16bit）の左上 x（アトラス座標）。64 ではなく 128（`xr_transfer_app.cpp` の `kBlockOrgBX`）。 */
export const M2_BLOCK_ORG_BX = 128;
/** 両ブロックの左上 y（アトラス座標）。 */
export const M2_BLOCK_ORG_Y = 64;
/** 眼マーカーの一辺（画素）。 */
export const M2_MARKER_SIZE = 32;
/** 左マーカー（マゼンタ）の左上 x（アトラス座標）。 */
export const M2_MARKER_LEFT_X = 160;
/** 右マーカー（シアン）の左上 x（右半分起点からの差分）。 */
export const M2_MARKER_RIGHT_DX = 160;
/** 両マーカーの左上 y（アトラス座標）。 */
export const M2_MARKER_Y = 8;

/** 位置の量子化範囲（±4m を 8bit に写像）。 */
export const M2_POS_MIN = -4.0;
export const M2_POS_MAX = 4.0;
/** 姿勢成分の量子化範囲（±1 を 8bit に写像）。 */
export const M2_ORI_MIN = -1.0;
export const M2_ORI_MAX = 1.0;

/**
 * 値を 0〜255 に丸める。C++ の `QuantizeByte` と同じ式であること。
 *
 * @param v - 量子化する値。
 * @param lo - 範囲の下端。
 * @param hi - 範囲の上端。
 * @returns 0〜255 の整数。範囲外は端に寄せる。
 */
export function quantizeByte(v: number, lo: number, hi: number): number {
  const t = ((v - lo) / (hi - lo)) * 255;
  if (!(t >= 0)) return 0;
  if (!(t <= 255)) return 255;
  return Math.floor(t + 0.5);
}

/**
 * 位置成分を量子化する。
 *
 * @param v - 位置成分（メートル）。
 * @returns 0〜255 の整数。
 */
export function quantizePos(v: number): number {
  return quantizeByte(v, M2_POS_MIN, M2_POS_MAX);
}

/**
 * 姿勢（クォータニオン）成分を量子化する。
 *
 * @param v - 姿勢成分。
 * @returns 0〜255 の整数。
 */
export function quantizeOri(v: number): number {
  return quantizeByte(v, M2_ORI_MIN, M2_ORI_MAX);
}

/** pose タグ計算に渡す片眼の pose。 */
export interface M2EyePose {
  readonly position: ArrayLike<number>;
  readonly orientation: ArrayLike<number>;
}

/**
 * renderId を FNV-1a 用の符号なし 64bit 整数に正規化する。
 *
 * @param renderId - パイプが運ぶ描画要求 ID（`XrRecord.renderId` は 10 進文字列）。
 * @returns 非負の整数値。
 * @throws `SyntaxError` — 文字列が整数として解釈できない場合。
 * @throws `RangeError` — 値が負の場合。
 */
function toRenderIdBigint(renderId: bigint | string | number): bigint {
  // ADR: number 経由では 2^53 を超える ID の下位 bit が欠けるため、呼び出し側は
  // パイプ由来の 10 進文字列か bigint を渡す。number は互換のためだけに受ける。
  const id = typeof renderId === 'number' ? BigInt(Math.trunc(renderId)) : BigInt(renderId);
  if (id < 0n) throw new RangeError('renderId は非負であること');
  return id;
}

/**
 * pose 要約タグ（ブロック B に載る値）。C++ の `PoseTag` と同じバイト列（計 30B:
 * renderId LE 8B、connEpoch LE 4B、spaceEpoch LE 4B、左 pos 3B、左 ori 4B、
 * 右 pos 3B、右 ori 4B）の FNV-1a 下位 16bit。
 *
 * @param renderId - 描画要求 ID 全体（タグは下位 16bit だけでなく全体を畳む）。
 * @param connEpoch - 接続世代。
 * @param spaceEpoch - 空間世代。
 * @param left - 左眼の pose。
 * @param right - 右眼の pose。
 * @returns 16bit のタグ値。
 * @throws `RangeError` — pose の要素数が 3（位置）・4（姿勢）でない場合。
 */
export function poseTag(
  renderId: bigint | string | number,
  connEpoch: number,
  spaceEpoch: number,
  left: M2EyePose,
  right: M2EyePose,
): number {
  if (left.position.length !== 3 || left.orientation.length !== 4) {
    throw new RangeError('左 pose の要素数が不正（pos=3・ori=4 であること）');
  }
  if (right.position.length !== 3 || right.orientation.length !== 4) {
    throw new RangeError('右 pose の要素数が不正（pos=3・ori=4 であること）');
  }
  const bytes: number[] = [];
  let id = toRenderIdBigint(renderId);
  for (let i = 0; i < 8; i++) {
    bytes.push(Number(id & 0xffn));
    id >>= 8n;
  }
  const conn = connEpoch >>> 0;
  const space = spaceEpoch >>> 0;
  for (let i = 0; i < 4; i++) bytes.push((conn >>> (i * 8)) & 0xff);
  for (let i = 0; i < 4; i++) bytes.push((space >>> (i * 8)) & 0xff);
  for (let i = 0; i < 3; i++) bytes.push(quantizePos(left.position[i]));
  for (let i = 0; i < 4; i++) bytes.push(quantizeOri(left.orientation[i]));
  for (let i = 0; i < 3; i++) bytes.push(quantizePos(right.position[i]));
  for (let i = 0; i < 4; i++) bytes.push(quantizeOri(right.orientation[i]));
  let hash = 2166136261 >>> 0;
  for (const b of bytes) {
    hash ^= b;
    hash = Math.imul(hash, 16777619) >>> 0;
  }
  return hash & 0xffff;
}

/**
 * renderId のブロック A 値を取る（下位 16bit）。
 *
 * @param renderId - 描画要求 ID 全体。
 * @returns 下位 16bit。
 */
export function renderId16(renderId: bigint | string | number): number {
  return Number(toRenderIdBigint(renderId) & 0xffffn);
}

/**
 * 16bit 値を 32x32 RGBA バッファへ書く。`m2_blocks.h` の `EncodeBlock16` と同じ
 * 配置（bit b は `((b%8)*4, floor(b/8)*4)` 起点の 4x4 セル。1=白・0=黒）。
 *
 * @param value - 16bit 値（0〜65535）。
 * @returns 32x32x4 バイトの画素列。
 * @throws `RangeError` — 値が 16bit 範囲外の場合。
 */
export function encodeBlock16(value: number): Uint8Array {
  if (!Number.isInteger(value) || value < 0 || value > 0xffff) {
    throw new RangeError('ブロック値は 0〜65535 の整数であること');
  }
  const px = new Uint8Array(M2_BLOCK_PIXELS * M2_BLOCK_PIXELS * 4);
  for (let b = 0; b < 16; b++) {
    const on = ((value >>> b) & 1) === 1;
    const x0 = (b % 8) * M2_BLOCK_CELL;
    const y0 = Math.floor(b / 8) * M2_BLOCK_CELL;
    const c = on ? 255 : 0;
    for (let dy = 0; dy < M2_BLOCK_CELL; dy++) {
      for (let dx = 0; dx < M2_BLOCK_CELL; dx++) {
        const o = ((y0 + dy) * M2_BLOCK_PIXELS + (x0 + dx)) * 4;
        px[o] = c;
        px[o + 1] = c;
        px[o + 2] = c;
        px[o + 3] = 255;
      }
    }
  }
  return px;
}

/**
 * 32x32 RGBA バッファから 16bit 値を復号する。各セルの中心画素だけを見て
 * しきい値 128 で二値化する（`DecodeBlock16` と同じ）。
 *
 * @param px - 32x32x4 バイトの画素列。
 * @returns 復号した 16bit 値。
 * @throws `RangeError` — バッファが 32x32x4 未満の場合。
 */
export function decodeBlock16(px: ArrayLike<number>): number {
  if (px.length < M2_BLOCK_PIXELS * M2_BLOCK_PIXELS * 4) {
    throw new RangeError('ブロックバッファが 32x32x4 未満');
  }
  let v = 0;
  for (let b = 0; b < 16; b++) {
    const cx = (b % 8) * M2_BLOCK_CELL + 2;
    const cy = Math.floor(b / 8) * M2_BLOCK_CELL + 2;
    const o = (cy * M2_BLOCK_PIXELS + cx) * 4;
    const avg = (px[o] + px[o + 1] + px[o + 2]) / 3;
    if (avg > 128) v |= 1 << b;
  }
  return v >>> 0;
}
