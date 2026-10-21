/**
 * コントロールウィンドウ用の preload（分離ワールド）。
 *
 * 公開するのはミラーの受信（画像・状態）と OSR への入力送信だけであり、
 * Node の機能は渡さない。OSR 側の `preload.ts`（XR ブリッジ）とは別物である。
 */
import { contextBridge, ipcRenderer } from 'electron';

/** main から届くミラーの 1 フレーム。data は BGRA 生配列（`capturePage().toBitmap()`）。 */
export interface MirrorFrame {
  readonly width: number;
  readonly height: number;
  readonly data: Uint8Array;
}

/** main から届く状態行。座標変換に OSR 寸法も含む。 */
export interface MirrorStatus {
  readonly connected: boolean;
  readonly vr: boolean;
  readonly fps: number;
  readonly mirrorFps: number;
  readonly captureMsAvg: number;
  readonly osrW: number;
  readonly osrH: number;
}

type FrameCallback = (frame: MirrorFrame) => void;
type StatusCallback = (status: MirrorStatus) => void;

const bridge = {
  onFrame(cb: FrameCallback): void {
    ipcRenderer.on('mirror:frame', (_event, frame: MirrorFrame) => cb(frame));
  },
  onStatus(cb: StatusCallback): void {
    ipcRenderer.on('mirror:status', (_event, status: MirrorStatus) => cb(status));
  },
  sendInput(msg: unknown): void {
    ipcRenderer.send('mirror:input', msg);
  },
};

contextBridge.exposeInMainWorld('__xriftMirror', bridge);
