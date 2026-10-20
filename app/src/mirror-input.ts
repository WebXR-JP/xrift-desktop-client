/**
 * コントロールウィンドウからの入力転送の境界検証と OSR 用イベントへの変換。
 *
 * renderer からの IPC 引数は `unknown` として受け、ここで形状・値域を確認してから
 * Electron の `sendInputEvent` に渡せる形へ直す。不正な入力は `null` で捨て、
 * 呼び出し側が数えて記録する（握り潰さない）。
 *
 * @packageDocumentation
 */
import type { KeyboardInputEvent, MouseInputEvent, MouseWheelInputEvent } from 'electron';

/** `sendInputEvent` に渡せる入力イベント。 */
export type OsrInputEvent = MouseInputEvent | MouseWheelInputEvent | KeyboardInputEvent;

/** コントロール renderer が送る入力メッセージ。座標は OSR content 画素（未丸め可）。 */
export type MirrorInputMsg =
  | { readonly kind: 'mouseMove'; readonly x: number; readonly y: number }
  | { readonly kind: 'mouseDown'; readonly x: number; readonly y: number; readonly button: 'left' | 'middle' | 'right'; readonly clickCount: number }
  | { readonly kind: 'mouseUp'; readonly x: number; readonly y: number; readonly button: 'left' | 'middle' | 'right'; readonly clickCount: number }
  | { readonly kind: 'mouseWheel'; readonly x: number; readonly y: number; readonly deltaX: number; readonly deltaY: number }
  | { readonly kind: 'keyDown'; readonly keyCode: string; readonly modifiers: readonly string[] }
  | { readonly kind: 'keyUp'; readonly keyCode: string; readonly modifiers: readonly string[] }
  | { readonly kind: 'char'; readonly keyCode: string };

/** `sendInputEvent` の modifiers に載せられる名前。 */
const ALLOWED_MODIFIERS = new Set([
  'shift', 'control', 'ctrl', 'alt', 'meta', 'command', 'cmd',
  'capslock', 'numlock', 'left', 'right', 'iskeypad', 'isautorepeat',
]);

function finiteNum(v: unknown): v is number {
  return typeof v === 'number' && Number.isFinite(v);
}

/** OSR 面内に収め、整数画素へ丸める。範囲外は面内へ寄せる（黒帯誤差の吸収）。 */
function clampPoint(x: unknown, y: unknown, osrW: number, osrH: number): { x: number; y: number } | null {
  if (!finiteNum(x) || !finiteNum(y)) return null;
  if (!(osrW > 0) || !(osrH > 0)) return null;
  const cx = Math.min(osrW - 1, Math.max(0, Math.round(x)));
  const cy = Math.min(osrH - 1, Math.max(0, Math.round(y)));
  return { x: cx, y: cy };
}

function buttonOf(v: unknown): 'left' | 'middle' | 'right' | null {
  return v === 'left' || v === 'middle' || v === 'right' ? v : null;
}

function clickCountOf(v: unknown): number {
  if (!finiteNum(v)) return 1;
  const n = Math.trunc(v);
  return n >= 1 && n <= 10 ? n : 1;
}

function keyCodeOf(v: unknown): string | null {
  if (typeof v !== 'string' || v === '' || v.length > 32) return null;
  return v;
}

function modifiersOf(v: unknown): Array<'shift' | 'control' | 'ctrl' | 'alt' | 'meta' | 'command' | 'cmd' | 'iskeypad' | 'isautorepeat' | 'left' | 'right' | 'capslock' | 'numlock'> {
  if (!Array.isArray(v)) return [];
  const out: Array<'shift' | 'control' | 'ctrl' | 'alt' | 'meta' | 'command' | 'cmd' | 'iskeypad' | 'isautorepeat' | 'left' | 'right' | 'capslock' | 'numlock'> = [];
  for (const m of v) {
    // ADR: 取り違え防止のため未知の修飾子は落とす（Electron は未知値を無視するが、
    // 送り側の綴り誤りを記録に残すためここで選別する）。
    if (typeof m === 'string' && ALLOWED_MODIFIERS.has(m.toLowerCase())) {
      const lower = m.toLowerCase() as (typeof out)[number];
      if (!out.includes(lower)) out.push(lower);
    }
  }
  return out;
}

/**
 * renderer 由来の入力を OSR 用イベントへ直す。
 *
 * @param msg - IPC で受けた値（`unknown` 境界）。
 * @param osrW - OSR content 幅（画素）。0 以下では全て捨てる。
 * @param osrH - OSR content 高さ（画素）。
 * @returns 送れる形のイベント。不正なら `null`。
 */
export function toOsrInputEvent(msg: unknown, osrW: number, osrH: number): OsrInputEvent | null {
  if (msg === null || typeof msg !== 'object') return null;
  const rec = msg as Record<string, unknown>;
  switch (rec.kind) {
    case 'mouseMove': {
      const p = clampPoint(rec.x, rec.y, osrW, osrH);
      if (p === null) return null;
      return { type: 'mouseMove', x: p.x, y: p.y };
    }
    case 'mouseDown':
    case 'mouseUp': {
      const p = clampPoint(rec.x, rec.y, osrW, osrH);
      const button = buttonOf(rec.button);
      if (p === null || button === null) return null;
      return {
        type: rec.kind === 'mouseDown' ? 'mouseDown' : 'mouseUp',
        x: p.x, y: p.y, button, clickCount: clickCountOf(rec.clickCount),
      };
    }
    case 'mouseWheel': {
      const p = clampPoint(rec.x, rec.y, osrW, osrH);
      if (p === null || !finiteNum(rec.deltaX) || !finiteNum(rec.deltaY)) return null;
      const deltaX = Math.min(100000, Math.max(-100000, Math.trunc(rec.deltaX)));
      const deltaY = Math.min(100000, Math.max(-100000, Math.trunc(rec.deltaY)));
      return { type: 'mouseWheel', x: p.x, y: p.y, deltaX, deltaY };
    }
    case 'keyDown':
    case 'keyUp': {
      const keyCode = keyCodeOf(rec.keyCode);
      if (keyCode === null) return null;
      return {
        type: rec.kind === 'keyDown' ? 'keyDown' : 'keyUp',
        keyCode, modifiers: modifiersOf(rec.modifiers),
      };
    }
    case 'char': {
      const keyCode = keyCodeOf(rec.keyCode);
      if (keyCode === null) return null;
      // ADR: char に修飾子を載せると印字が化けることがあるため落とす。
      return { type: 'char', keyCode };
    }
    default:
      return null;
  }
}
