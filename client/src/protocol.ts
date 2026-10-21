/**
 * パイププロトコルの配置と復号。配置は `native/src/transfer/protocol.h` が正本であり、
 * ここは main プロセス側の読み取り専用の写しである。配置を変えたら両方を同時に更新する。
 *
 * @packageDocumentation
 */

/** 1 メッセージの固定長（バイト）。256B 固定である。 */
export const MSG_LEN = 256;
/** メッセージ先頭の識別子。 */
export const MAGIC = 0x54465258;
/** プロトコル版。256B 化で 3 に上げた。 */
export const VERSION = 3;

export const TYPE_OFFER = 1;
export const TYPE_DONE = 2;
export const TYPE_RENDER_REQUEST = 3;
export const TYPE_HAPTIC_COMMAND = 4;
export const TYPE_INPUT_STATE = 5;

export const FORMAT_BGRA = 0;
export const STATUS_OK = 0;

/** exe が発行する描画要求。pose/FOV は描画要求であり、main は値を推測・付与しない。 */
export interface XrRecord {
  readonly renderId: string;
  readonly connEpoch: number;
  readonly spaceEpoch: number;
  readonly leftPos: readonly number[];
  readonly leftOri: readonly number[];
  readonly rightPos: readonly number[];
  readonly rightOri: readonly number[];
  readonly leftFov: readonly number[];
  readonly rightFov: readonly number[];
  /** 要求描画サイズ（片眼）。実寸と食い違ったら実寸を正とする。 */
  readonly reqWidth: number;
  readonly reqHeight: number;
  /** 旧 pose 混入の合図（検証フック用）。通常は false。 */
  readonly staleHint: boolean;
}

/** コントローラの pose（grip または aim）。無効時は 0 埋めであり、シムは `null` を返す。 */
export interface ControllerPose {
  /** pose が locate できたか。手 flags（bit1=grip・bit2=aim）が正本である。 */
  readonly valid: boolean;
  /** 位置（メートル）。無効時は `[0, 0, 0]`。 */
  readonly position: readonly [number, number, number];
  /** 姿勢（xyzw）。無効時は `[0, 0, 0, 0]`。 */
  readonly orientation: readonly [number, number, number, number];
}

/** 片手の入力。pose の有効性は `grip.valid`・`aim.valid` で表す。 */
export interface InputHand {
  readonly connected: boolean;
  readonly profile: string;
  /** grip 空間の pose。シムの `gripSpace` の源である。 */
  readonly grip: ControllerPose;
  /** aim 空間の pose。シムの `targetRaySpace` の源である。 */
  readonly aim: ControllerPose;
  readonly trigger: number;
  readonly squeeze: number;
  readonly thumbstickClick: boolean;
  readonly trackpadClick: boolean;
  readonly primary: boolean;
  readonly secondary: boolean;
  readonly menu: boolean;
  readonly thumbstick: readonly [number, number];
  readonly trackpad: readonly [number, number];
  readonly hasThumbstick: boolean;
  readonly hasTrackpad: boolean;
}

/** exe からの入力スナップショット。 */
export interface InputSnapshot {
  readonly frame: string;
  readonly connEpoch: number;
  readonly spaceEpoch: number;
  readonly left: InputHand;
  readonly right: InputHand;
  /** その 1 秒間に押されたボタンの集合（予約域のラッチ）。 */
  readonly latchLeft: string;
  readonly latchRight: string;
}

const PROFILE_NAMES: Readonly<Record<number, string>> = {
  0: '-',
  1: 'simple',
  2: 'oculus',
  3: 'index',
  4: 'vive',
  5: 'motion',
  6: 'pico4',
  7: 'pico-neo3',
  8: 'pico-ultra',
  9: 'pico-g3',
  255: 'other',
};

const BUTTON_NAMES = ['stick', 'pad', 'primary', 'secondary', 'menu'] as const;

function floats(m: Buffer, off: number, n: number): number[] {
  return Array.from({ length: n }, (_, i) => m.readFloatLE(off + i * 4));
}

function decodeLatch(bits: number): string {
  const names: string[] = [];
  for (let b = 0; b < 5; b++) {
    if ((bits & (1 << b)) !== 0) names.push(BUTTON_NAMES[b]);
  }
  return names.length > 0 ? names.join(',') : '-';
}

/**
 * InputState 内の pose 1 件を復号する。
 *
 * @param m - 256 バイトのメッセージ本体。
 * @param off - pose のオフセット（左 grip=88・左 aim=116・右 grip=160・右 aim=188）。
 * @param valid - 手 flags の対応 bit（grip=bit1・aim=bit2）。偽のとき値は読まない。
 * @returns 復号した pose。無効時は 0 埋めである。
 */
function decodePose(m: Buffer, off: number, valid: boolean): ControllerPose {
  if (!valid) {
    return { valid: false, position: [0, 0, 0], orientation: [0, 0, 0, 0] };
  }
  return {
    valid: true,
    position: [m.readFloatLE(off), m.readFloatLE(off + 4), m.readFloatLE(off + 8)],
    orientation: [
      m.readFloatLE(off + 12),
      m.readFloatLE(off + 16),
      m.readFloatLE(off + 20),
      m.readFloatLE(off + 24),
    ],
  };
}

function decodeHand(m: Buffer, base: number, gripOff: number, aimOff: number): InputHand {
  const flags = m[base];
  const bits = m[base + 8];
  const btnSupported = m[base + 12];
  const analogSupported = m[base + 13];
  const pressed = (bit: number): boolean => (bits & (1 << bit)) !== 0;
  // analogSupported の bit 割付けは bit0=trigger・bit1=squeeze・bit2=stick・bit3=pad。
  const stickActive = (m[base + 11] & 1) !== 0;
  return {
    connected: (flags & 1) !== 0,
    profile: PROFILE_NAMES[m[base + 1]] ?? `id=${m[base + 1]}`,
    grip: decodePose(m, gripOff, (flags & 2) !== 0),
    aim: decodePose(m, aimOff, (flags & 4) !== 0),
    trigger: m[base + 2] / 255,
    squeeze: m[base + 3] / 255,
    thumbstickClick: pressed(0),
    trackpadClick: pressed(1),
    primary: pressed(2),
    secondary: pressed(3),
    menu: pressed(4),
    thumbstick: [m.readInt8(base + 4) / 127, m.readInt8(base + 5) / 127],
    trackpad: [m.readInt8(base + 6) / 127, m.readInt8(base + 7) / 127],
    // 非活性と未対応を区別する。active でなければ対応表を見て文言側で表す。
    hasThumbstick: stickActive || (analogSupported & 4) !== 0,
    hasTrackpad: (analogSupported & 8) !== 0 || (btnSupported & 2) !== 0,
  };
}

/**
 * RenderRequest メッセージを復号する。
 *
 * @param m - 256 バイトのメッセージ本体。
 * @returns 復号したレコード。不正な配置のときは `null`。
 */
export function decodeRecord(m: Buffer): XrRecord | null {
  if (m.length < MSG_LEN) return null;
  return {
    renderId: m.readBigUInt64LE(40).toString(),
    connEpoch: m.readUInt32LE(48),
    spaceEpoch: m.readUInt32LE(52),
    leftPos: floats(m, 56, 3),
    leftOri: floats(m, 68, 4),
    rightPos: floats(m, 84, 3),
    rightOri: floats(m, 96, 4),
    leftFov: floats(m, 112, 4),
    rightFov: floats(m, 128, 4),
    reqWidth: m.readUInt32LE(144),
    reqHeight: m.readUInt32LE(148),
    staleHint: m[152] === 0x5a,
  };
}

/**
 * InputState メッセージを復号する。
 *
 * @param m - 256 バイトのメッセージ本体。
 * @returns 復号したスナップショット。不正な配置のときは `null`。
 */
export function decodeInput(m: Buffer): InputSnapshot | null {
  if (m.length < MSG_LEN) return null;
  return {
    frame: m.readBigUInt64LE(40).toString(),
    connEpoch: m.readUInt32LE(48),
    spaceEpoch: m.readUInt32LE(52),
    left: decodeHand(m, 56, 88, 116),
    right: decodeHand(m, 72, 160, 188),
    latchLeft: decodeLatch(m[152]),
    latchRight: decodeLatch(m[153]),
  };
}

/**
 * Offer メッセージを作る。
 *
 * @param borrowId - 借用番号。
 * @param width - 画像幅。
 * @param height - 画像高さ。
 * @param duplicated - exe プロセスへ複製済みのハンドル値。
 */
export function makeOffer(borrowId: number, width: number, height: number, duplicated: bigint): Buffer {
  const buf = Buffer.alloc(MSG_LEN);
  buf.writeUInt32LE(MAGIC, 0);
  buf.writeUInt16LE(VERSION, 4);
  buf.writeUInt16LE(TYPE_OFFER, 6);
  buf.writeBigUInt64LE(BigInt(borrowId), 8);
  buf.writeUInt32LE(width, 16);
  buf.writeUInt32LE(height, 20);
  buf.writeUInt32LE(FORMAT_BGRA, 24);
  buf.writeBigUInt64LE(duplicated, 28);
  buf.writeUInt32LE(0, 36);
  return buf;
}

/**
 * HapticCommand メッセージを作る。配置は `MakeHapticCommand` と同じ。
 *
 * @param hand - 0=左、1=右。
 * @param amplitude - 振幅。
 * @param durationMs - 持続時間（ミリ秒）。
 */
export function makeHaptic(hand: number, amplitude: number, durationMs: number): Buffer {
  const buf = Buffer.alloc(MSG_LEN);
  buf.writeUInt32LE(MAGIC, 0);
  buf.writeUInt16LE(VERSION, 4);
  buf.writeUInt16LE(TYPE_HAPTIC_COMMAND, 6);
  buf.writeUInt32LE(hand, 40);
  buf.writeFloatLE(amplitude, 56);
  buf.writeUInt32LE(durationMs, 48);
  return buf;
}
