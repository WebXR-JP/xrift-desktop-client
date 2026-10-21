/**
 * preload（分離ワールド）。ページには XR 用の最小ブリッジだけを公開する。
 *
 * 公開するのは描画要求・入力・振動・到達確認だけであり、Node の機能は渡さない。
 * WebXR シム（`./shim`）は `executeInMainWorld` でページより先に注入する。
 */
import { contextBridge, ipcRenderer } from 'electron';
import { installM5XR } from './shim.js';

/** renderer へ送る描画要求。構造は `protocol.ts` の `XrRecord` と同じ。 */
export interface RecordPayload {
  readonly renderId: string;
  readonly connEpoch: number;
  readonly spaceEpoch: number;
  readonly leftPos: readonly number[];
  readonly leftOri: readonly number[];
  readonly rightPos: readonly number[];
  readonly rightOri: readonly number[];
  readonly leftFov: readonly number[];
  readonly rightFov: readonly number[];
  readonly reqWidth: number;
  readonly reqHeight: number;
  readonly staleHint: boolean;
}

/** renderer へ送る入力。構造は `protocol.ts` の `InputSnapshot` と同じ。 */
export interface InputPayload {
  readonly frame: string;
  readonly connEpoch: number;
  readonly spaceEpoch: number;
  readonly left: unknown;
  readonly right: unknown;
  readonly latchLeft: string;
  readonly latchRight: string;
}

type RecordCallback = (rec: RecordPayload) => void;
type InputCallback = (snap: InputPayload) => void;

const bridge = {
  onRecord(cb: RecordCallback): void {
    ipcRenderer.on('xr:record', (_event, rec: RecordPayload) => cb(rec));
  },
  onInput(cb: InputCallback): void {
    ipcRenderer.on('xr:input', (_event, snap: InputPayload) => cb(snap));
  },
  haptic(hand: number, intensity: number, durationMs: number): void {
    ipcRenderer.send('xr:haptic', hand, intensity, durationMs);
  },
  probe(): Promise<{ ok: boolean }> {
    return ipcRenderer.invoke('xr:probe');
  },
  sessionActive(active: boolean): void {
    ipcRenderer.send('xr:session', active);
  },
  // シムの初回制御ブロック描画の通知。main はこの通知まで Offer しない。
  // 境界は文字列だけを受け、他の型は送らない。
  stamped(renderId: string): void {
    if (typeof renderId !== 'string' || renderId === '') return;
    ipcRenderer.send('xr:stamped', renderId);
  },
};

contextBridge.exposeInMainWorld('__xriftXR', bridge);
// ページのスクリプトより先に navigator.xr を差し替える
contextBridge.executeInMainWorld({ func: installM5XR });
