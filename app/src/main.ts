/**
 * M5 新アプリの main プロセス。検証用ハーネス（`native_new/harness/electron/main.js`）の
 * 借用台帳・パイプサーバ・終了経路を TypeScript に整理して流用したものである。
 *
 * 役割: パイプサーバ + 借用台帳 + `release()` 専任。提出には関与しない。
 *  - OSR ウィンドウ（show:false、offscreen useSharedTexture）で paint を受ける
 *  - 借用は処理中 1 枚まで。未処理の Offer がある間の paint は stale として即 release する
 *  - Done を受けたらその借用だけ release する（二重 release しない）
 *  - exe の終了・切断時は、未 offer の保留だけ release する。offer 済み未 ACK は
 *    GPU 完了の証拠が無いため release せず隔離する。Electron 終了で回収する
 *  - `RenderRequest` は renderer へそのまま渡す（pose/FOV を推測・付与しない）。
 *    `InputState` も同様に渡す。`HapticCommand` は renderer からの IPC を中継する
 *
 * @packageDocumentation
 */
import { app, BrowserWindow, ipcMain, type NativeImage } from 'electron';
import { spawn, type ChildProcess } from 'node:child_process';
import * as net from 'node:net';
import * as path from 'node:path';
import * as fs from 'node:fs';
import {
  MSG_LEN, MAGIC, VERSION,
  TYPE_DONE, TYPE_RENDER_REQUEST, TYPE_INPUT_STATE,
  STATUS_OK, FORMAT_BGRA,
  decodeRecord, decodeInput, makeOffer, makeHaptic,
  type InputSnapshot, type XrRecord,
} from './protocol';
import { toOsrInputEvent } from './mirror-input';

// VR 中はデスクトップのウィンドウが隠れていても描画・タイマーを止めない。
// appendSwitch は ready より前に呼ぶ（ready 後に足しても効かない）。
app.commandLine.appendSwitch('disable-background-timer-throttling');
app.commandLine.appendSwitch('disable-renderer-backgrounding');
app.commandLine.appendSwitch('disable-backgrounding-occluded-windows');
app.commandLine.appendSwitch('disable-features', 'CalculateNativeWinOcclusion');
app.commandLine.appendSwitch('autoplay-policy', 'no-user-gesture-required');

// パッケージ版（app.isPackaged）では argv にスクリプトパスが含まれないため、
// 開発時（electron . …）より 1 つ前から読む。SteamVR からの起動引数もここに載る。
// ADR: slice 位置を各所で直書きせず ARGV に一本化する（ずれると --self-test 等の
// フラグが黙って無視され、通常起動として固まる）。
const ARGV: readonly string[] = process.argv.slice(app.isPackaged ? 1 : 2);
function arg(name: string, def: string): string {
  const prefix = `--${name}=`;
  for (const a of ARGV) {
    if (a.startsWith(prefix)) return a.slice(prefix.length);
  }
  return def;
}

// パッケージ版では exe/addon を extraResource（process.resourcesPath 直下）から読む。
// 開発時は従来の native_new 配下に退避する。--exe/--addon の明示指定が最優先である。
// ADR: __dirname は開発時もパッケージ版も resources/app/dist を指すため、
// pages/preload 等の相対解決は変えない。変わるのは exe/addon の既定値だけである。
function packagedResource(fileName: string): string | null {
  try {
    const candidate = path.join(process.resourcesPath, fileName);
    if (fs.existsSync(candidate)) return candidate;
  } catch { /* 開発実行では resourcesPath が無い場合がある */ }
  return null;
}
const DEV_EXE_PATH = path.join(__dirname, '..', '..', 'native_new', 'build', 'windows', 'Release', 'xrift_new.exe');
const DEV_ADDON_PATH = path.join(
  __dirname, '..', '..', 'native_new', 'transfer-addon', 'build', 'Release', 'xrift_transfer_addon.node',
);
const EXE_PATH = arg('exe', packagedResource('xrift_new.exe') ?? DEV_EXE_PATH);
const PIPE_PATH = arg('pipe', `\\\\.\\pipe\\xrift_m5_${process.pid}`);
const RENDER_SCALE = arg('render-scale', '0.5');
const RECORD_TTL_MS = Math.max(1, parseInt(arg('record-ttl-ms', '2000'), 10) || 2000);
const SECONDS = Math.max(0, parseInt(arg('seconds', '0'), 10) || 0);
// C 案の OSR はブラウジング中 1280x800。VR 開始（RenderRequest）で
// 2*eye × eye へ広げる（forwardRecord が担う）。--width/--height で上書き可。
const WIN_W = parseInt(arg('width', '1280'), 10) || 1280;
const WIN_H = parseInt(arg('height', '800'), 10) || 800;
const SPACE = arg('space', 'auto');
const PAGE_ARG = arg('page', '');
const URL_ARG = arg('url', '');
/**
 * 開く対象を決める。`--url` が正典であり、`--page` は互換のために残す。
 *
 * - `https?://`・`file://` で始まればそのまま URL として開く
 * - それ以外の空でない文字（`pages/test.html`・`test.html` 等）は pages 配下の
 *   ファイルとして開く（`--url=pages/test.html` の検証命令に対応するため）
 * - どちらも空なら既定の `pages/test.html`
 */
function resolveTarget(): { kind: 'url'; url: string } | { kind: 'file'; file: string } {
  const raw = URL_ARG !== '' ? URL_ARG : PAGE_ARG !== '' ? PAGE_ARG : 'test.html';
  if (/^https?:\/\//i.test(raw) || /^file:\/\//i.test(raw)) {
    return { kind: 'url', url: raw };
  }
  if (path.isAbsolute(raw) && fs.existsSync(raw)) {
    return { kind: 'file', file: raw };
  }
  const pagesBase = path.join(__dirname, '..', 'pages');
  const candidates = [
    path.join(pagesBase, path.basename(raw)),
    path.join(pagesBase, raw),
    path.join(__dirname, '..', raw),
  ];
  for (const c of candidates) {
    try {
      if (fs.existsSync(c) && fs.statSync(c).isFile()) return { kind: 'file', file: c };
    } catch { /* 次の候補へ */ }
  }
  // ファイルとして見つからなければ URL として扱う（到達失敗は did-fail-load に残る）。
  if (/^[a-z][a-z0-9+.-]*:/i.test(raw)) return { kind: 'url', url: raw };
  return { kind: 'file', file: candidates[0] };
}
const ADDON_PATH = arg('addon', packagedResource('xrift_transfer_addon.node') ?? DEV_ADDON_PATH);
// exe の stdout/stderr の保存先（受理/不明 ID の内訳用）。相対パスは logs/ 基準。
// `--exe-log=`（空）で従来の inherit（保存しない）に戻す。
const EXE_LOG = arg('exe-log', 'm5-exe.log');
if (!['auto', 'local', 'stage', 'local-floor'].includes(SPACE)) {
  console.error(`unknown --space=${SPACE}（auto / local / stage / local-floor のいずれか）`);
  process.exitCode = 1;
  app.whenReady().then(() => app.exit(1));
}
// 検証専用の画面 capture（--capture-at=Ms,path。複数は ; 区切り）。
// 指定時刻に webContents.capturePage() で PNG を保存する。描画経路には入れない。
const CAPTURE_AT_LIST: { ms: number; file: string }[] = (() => {
  const raw = arg('capture-at', '');
  if (raw === '') return [];
  const out: { ms: number; file: string }[] = [];
  for (const s of raw.split(';')) {
    const t = s.trim();
    if (t === '') continue;
    const i = t.indexOf(',');
    if (i <= 0) continue;
    const ms = Math.max(0, parseInt(t.slice(0, i).trim(), 10) || 0);
    const file = t.slice(i + 1).trim();
    if (!(ms > 0) || file === '') continue;
    out.push({ ms, file });
  }
  return out;
})();
// C 案（OSR のミラー + 入力転送）の設定。exe 無しでもミラー検証ができるよう
// `--no-exe` では exe の spawn・パイプサーバを飛ばす。`--self-test` は
// ミラー・入力転送・VR リサイズの自動検証を行い、結果で終了する（HMD 不要）。
// 検証専用の pace 無効化（`--no-pace-records`）。exe への `--pace-records` を
// 渡さず、従来どおり毎フレーム発行させる。遅いページでの受理率の on/off 比較用。
// 通常運用では付けない（既定は pace on）。
const NO_PACE = ARGV.includes('--no-pace-records');
const NO_EXE = ARGV.includes('--no-exe');
const SELF_TEST = ARGV.includes('--self-test');
const MIRROR_FPS = Math.min(30, Math.max(1, parseInt(arg('mirror-fps', '10'), 10) || 10));
// ADR: VR 中のミラーは既定で停止する（0=停止）。ブラウジング中は 10fps のまま。
// 0 を許すため下限は 0 とし、数値以外は既定 0 に倒す。
const MIRROR_VR_FPS = (() => {
  const raw = arg('mirror-vr-fps', '0');
  const n = parseInt(raw, 10);
  if (!Number.isFinite(n)) return 0;
  return Math.min(30, Math.max(0, Math.trunc(n)));
})();
const CTL_W = parseInt(arg('control-width', '960'), 10) || 960;
const CTL_H = parseInt(arg('control-height', '600'), 10) || 600;
// 検証専用のコントロール窓 capture（--control-capture-at=Ms,path。複数は ; 区切り）。
const CONTROL_CAPTURE_AT_LIST: { ms: number; file: string }[] = (() => {
  const raw = arg('control-capture-at', '');
  if (raw === '') return [];
  const out: { ms: number; file: string }[] = [];
  for (const s of raw.split(';')) {
    const t = s.trim();
    if (t === '') continue;
    const i = t.indexOf(',');
    if (i <= 0) continue;
    const ms = Math.max(0, parseInt(t.slice(0, i).trim(), 10) || 0);
    const file = t.slice(i + 1).trim();
    if (!(ms > 0) || file === '') continue;
    out.push({ ms, file });
  }
  return out;
})();

// ---- ログ（stdout とファイルの両方。ランチャー経由だとコンソールが無いため）
const logDir = path.join(__dirname, '..', 'logs');
fs.mkdirSync(logDir, { recursive: true });
const logPath = path.join(logDir, 'm5-run.log');
const statsPath = path.join(logDir, 'm5-stats.json');
const logLines: string[] = [];
function log(msg: string): void {
  const line = `[${new Date().toISOString()}] ${msg}`;
  logLines.push(line);
  try {
    console.log(line);
  } catch {
    // 親パイプ切断時（EPIPE）はファイル側の記録を優先し、落とさない
  }
}
function flushLog(): void {
  try {
    fs.writeFileSync(logPath, logLines.join('\n') + '\n');
  } catch { /* 終了時は無視 */ }
}
process.on('uncaughtException', (e: unknown) => {
  if (e !== null && typeof e === 'object' && 'code' in e && (e as { code: unknown }).code === 'EPIPE') return;
  log('FATAL uncaughtException: ' + (e instanceof Error ? e.stack : String(e)));
  flushLog();
  app.exit(1);
});

// ---- transfer-addon の読み込み（境界で unknown として受け、形状を検証する）
interface TransferAddon {
  duplicateHandle(handle: Buffer, targetPid: number): { ok: boolean; handle?: bigint };
}
function asTransferAddon(v: unknown): TransferAddon | null {
  if (v === null || typeof v !== 'object') return null;
  const dup = (v as Record<string, unknown>).duplicateHandle;
  if (typeof dup !== 'function') return null;
  return v as TransferAddon;
}
// eslint-disable-next-line @typescript-eslint/no-require-imports -- preload と同じく main でのみ addon を読む
let addon: TransferAddon | null = null;
try {
  // ADR: require の戻りは型が無いため unknown 経由で検証する。any 境界にしない。
  const mod: unknown = require(ADDON_PATH);
  addon = asTransferAddon(mod);
  if (addon === null) throw new Error('duplicateHandle が無い');
  log(`addon loaded: ${ADDON_PATH}`);
} catch (e) {
  log('addon load FAILED: ' + (e instanceof Error ? e.message : String(e)));
  flushLog();
  process.exitCode = 1;
  app.whenReady().then(() => app.exit(1));
}

// ---- OSR 共有テクスチャの境界検証
interface SharedTexture {
  release(): void;
  readonly textureInfo: {
    readonly pixelFormat: string;
    readonly codedSize: { readonly width: number; readonly height: number };
    readonly handle: { readonly ntHandle: unknown };
  };
}
function asSharedTexture(v: unknown): SharedTexture | null {
  if (v === null || typeof v !== 'object') return null;
  const t = v as Record<string, unknown>;
  if (typeof t.release !== 'function') return null;
  const ti = t.textureInfo;
  if (ti === null || typeof ti !== 'object') return null;
  const info = ti as Record<string, unknown>;
  if (typeof info.pixelFormat !== 'string') return null;
  const coded = info.codedSize;
  if (coded === null || typeof coded !== 'object') return null;
  const codedRec = coded as Record<string, unknown>;
  if (typeof codedRec.width !== 'number' || typeof codedRec.height !== 'number') return null;
  const handle = info.handle;
  if (handle === null || typeof handle !== 'object') return null;
  if (!('ntHandle' in (handle as Record<string, unknown>))) return null;
  return v as SharedTexture;
}

// ---- 借用台帳。release() は main プロセスだけが呼び、1 借用 1 回に限る
let nextBorrowId = 1;
const held = new Map<number, SharedTexture>(); // borrowId -> texture（release 前に保持）
const quarantined = new Map<number, SharedTexture>(); // borrowId -> texture（完了未確認。触らない）
let inFlight: number | null = null; // offer 済み・ACK 待ちの borrowId（最大 1）
const MAX_HELD = 1; // 同時保持の上限（処理中 1 枚のみ。未処理がある間の paint は stale として即返す）
let stopping = false; // 計測終了後。新規 paint は即 release する
let finished = false;
const stats = {
  offers: 0, dones: 0, drops: 0, droppedStale: 0, offerFailures: 0, staleDones: 0,
  // 有効画素前の paint 破棄数（ブラウジング中・ページ初回描画待ちの旧内容）。
  droppedBrowsing: 0,
  quarantined: 0, releasedPending: 0, maxHeld: 0,
  statusOk: 0, statusCopyFailed: 0, statusVerifyFailed: 0,
  noTexture: 0, formatMismatch: 0, missingHandle: 0,
  distinctHandles: 0, firstCodedSize: null as { width: number; height: number } | null,
  renderRequests: 0, // exe から受けた RenderRequest の件数。
  inputStates: 0, // exe から受けた InputState の件数。
  lastInput: null as InputSnapshot | null, // 最後に受けた InputState の要約（統計用）。
  hapticSent: 0, // exe へ送った HapticCommand の件数。
  captures: 0, // 検証専用の --capture-at で保存した PNG の枚数（描画経路に影響しない）。
  controlCaptures: 0, // --control-capture-at・self-test で保存したコントロール窓 PNG の枚数。
  mirrorFrames: 0, // コントロール窓へ送ったミラーの枚数。
  mirrorErrors: 0, // ミラー capture・送信の失敗回数。
  mirrorCaptureAvgMs: 0, // capturePage の平均所要（終了時に確定）。
  mirrorCaptureMaxMs: 0, // capturePage の最大所要。
  inputForwarded: 0, // OSR へ転送した入力イベントの件数。
  inputErrors: 0, // 破棄・失敗した入力転送の件数。
  releaseAttempts: 0, // release() の試行回数。
};
const handleCounts = new Map<string, number>();
const paintTimes: number[] = [];
let exeProc: ChildProcess | null = null;
let socket: net.Socket | null = null;
// exe の stdout/stderr 保存先ストリーム。finish で end して書き切りを待つため
// start の局所変数ではなくここに置く（所有権は finish が引き取る）。
let exeLogStream: fs.WriteStream | null = null;
let winRef: BrowserWindow | null = null; // レコード転送先（createWindow が設定する）。
let stopTimer: NodeJS.Timeout | null = null;
let graceTimer: NodeJS.Timeout | null = null;
// beginShutdown の開始済み旗。--seconds とウィンドウ close の競合で
// 二重開始すると猶予タイマーが作り直されて終了が延びるため、2 回目以降は無視する。
// stopping（新規 paint の停止）とは別であり、優雅な停止の手順自体の多重起動を防ぐ。
let shutdownStarted = false;
// exe の stdout/stderr の行バッファ。チャンク境界で行が切れるため、
// 改行で区切れた完成行だけを統計抽出に使う。残りは finish 時に掃き出す。
let exeLineBuf = '';
// exe の終了統計（XR転送統計）を app ログに残したか。終了時の必須証拠である。
let exeStatsLogged = false;
const captureTimers: NodeJS.Timeout[] = [];
const controlCaptureTimers: NodeJS.Timeout[] = [];
// ---- C 案のミラー状態。OSR（不可視）の capturePage をタイマーで拾い、
// 可視のコントロール窓へ送る。VR 中は停止（既定）または節約間隔で VR の負荷を優先する。
let controlRef: BrowserWindow | null = null; // ミラー表示・入力受付先（createControlWindow が設定する）。
// ADR: VR の開始は renderer の requestSession 通知（xr:session）で決める。
// exe は起動時から RenderRequest を送り続けるため、RenderRequest の到着だけでは
// VR とみなさない。xrActive が false の間は VR サイズへ広げず renderer へも転送しない。
let xrActive = false; // renderer が requestSession したら真、セッション終了で偽。
let vrActive = false; // ミラー節約の旗。xrActive に追従する（開始で真、終了で偽）。
let xrDroppedRecords = 0; // xrActive が false の間に受けて捨てた RenderRequest の件数。
let mirrorTimer: NodeJS.Timeout | null = null;
let mirrorStopped = false; // 終了処理後は再開しない（タイマー解除の証跡）。
let mirrorNextAt = 0; // 次拍の予定時刻（固定レート維持用。遅延分を詰めて平均 fps を保つ）。
let statusTimer: NodeJS.Timeout | null = null;
let mirrorTotalMs = 0; // capturePage 所要の合計（平均用）。
const mirrorTimes: number[] = []; // ミラー送信時刻（直近の fps 計算用。100 件で丸める）。
// VR 中の 5 秒計測用。exe の stdout を main が中継する際に数える（提出・棄却の内訳）。
// paint レート（OSR 側の描画）と受理率（exe 側の照合）を切り分けるため、5 秒ごとにログに出す。
const vrMeter = {
  submits: 0, // exe の M2 提出（受理して blit へ進んだ件数）。
  unknownId: 0, // 棄却の内訳（保持に無い renderId）。
  poseMismatch: 0, // 棄却の内訳（pose タグ不一致）。
  spaceMismatch: 0, // 棄却の内訳（space 世代不一致）。
  expired: 0, // 棄却の内訳（TTL 超過）。
  lastPaints: 0, // 前回計測時の paints 累計（差分でレートを出す）。
  lastSubmits: 0, // 前回計測時の提出累計。
  lastUnknown: 0, lastPoseNg: 0, lastSpaceNg: 0, lastExpired: 0,
  lastStale: 0, // 前回計測時の stale paint 累計（差分で捨て率を出す）。
};
let vrMeterTimer: NodeJS.Timeout | null = null;
// exe の 1 チャンクから提出・棄却の行を数える。exe のログ形式
// （`M2 提出:`・`M2 棄却: <理由>`）に依存するため、形式が変わればここも更新する。
function countExeMeterChunk(text: string): void {
  if (!text.includes('M2 ')) return;
  const prevSubmits = vrMeter.submits;
  // 提出は 1 行 1 件である。
  let i = text.indexOf('M2 提出:');
  while (i >= 0) {
    vrMeter.submits++;
    i = text.indexOf('M2 提出:', i + 1);
  }
  // 有効化後の最初の受理までの時間を記録する（VR 開始の立ち上がり観測用）。
  if (xrActive && !vrFirstAcceptLogged && vrMeter.submits > prevSubmits && xrActiveSince !== null) {
    vrFirstAcceptLogged = true;
    const elapsed = Date.now() - xrActiveSince;
    const m = /M2 提出: renderId=(\d+)/.exec(text);
    const idNote = m !== null ? `（renderId=${m[1]}）` : '';
    log(`[M5 VR開始] 最初の受理まで ${elapsed}ms${idNote}`);
  }
  // 棄却は理由名で内訳を数える（render_record.h の RejectReasonName と一致）。
  const reasons: [string, 'unknownId' | 'poseMismatch' | 'spaceMismatch' | 'expired'][] = [
    ['M2 棄却: 不明ID', 'unknownId'],
    ['M2 棄却: pose不一致', 'poseMismatch'],
    ['M2 棄却: space世代不一致', 'spaceMismatch'],
    ['M2 棄却: 失効', 'expired'],
  ];
  for (const [needle, key] of reasons) {
    let j = text.indexOf(needle);
    while (j >= 0) {
      vrMeter[key]++;
      j = text.indexOf(needle, j + 1);
    }
  }
}
// exe の stdout/stderr チャンクから `XR転送統計` の完成行を抜き出して app ログに残す。
// 終了証拠としてウィンドウ close・--seconds のいずれでも必須であるため、
// 行の途中で切れた分は exeLineBuf に溜め、finish 時の掃き出しで回収する。
// 前提: exe の統計行は `XR転送統計:` を含む 1 行である（XrStats::Print の形式）。
function noteExeStatsChunk(text: string): void {
  if (!text.includes('XR転送統計') && !text.includes('\n')) {
    exeLineBuf += text;
    return;
  }
  exeLineBuf += text;
  const parts = exeLineBuf.split('\n');
  exeLineBuf = parts.pop() ?? '';
  for (const raw of parts) {
    const line = raw.replace(/\r$/, '');
    if (line.includes('XR転送統計')) {
      exeStatsLogged = true;
      log(`exe統計: ${line.slice(0, 2000)}`);
    }
  }
}
// finish 時の掃き出し。close（EOF）までに届いた末尾の断片を回収する。
function flushExeStats(): void {
  const line = exeLineBuf.replace(/\r$/, '');
  exeLineBuf = '';
  if (line.includes('XR転送統計')) {
    exeStatsLogged = true;
    log(`exe統計: ${line.slice(0, 2000)}`);
  }
}
// VR 中の 5 秒計測。paint レート（描画が遅いか）と受理率（転送が遅いか）を切り分ける。
// xrActive が false の間は出さない（ブラウジング中の雑音にしない）。
function logVrMeter(): void {
  if (!xrActive) return;
  const paints = paintTimes.length;
  const dPaints = paints - vrMeter.lastPaints;
  const dSub = vrMeter.submits - vrMeter.lastSubmits;
  const dUnk = vrMeter.unknownId - vrMeter.lastUnknown;
  const dPose = vrMeter.poseMismatch - vrMeter.lastPoseNg;
  const dSpace = vrMeter.spaceMismatch - vrMeter.lastSpaceNg;
  const dExp = vrMeter.expired - vrMeter.lastExpired;
  const dStale = stats.droppedStale - vrMeter.lastStale;
  vrMeter.lastPaints = paints;
  vrMeter.lastSubmits = vrMeter.submits;
  vrMeter.lastUnknown = vrMeter.unknownId;
  vrMeter.lastPoseNg = vrMeter.poseMismatch;
  vrMeter.lastSpaceNg = vrMeter.spaceMismatch;
  vrMeter.lastExpired = vrMeter.expired;
  vrMeter.lastStale = stats.droppedStale;
  // 受理率: exe が判定した件数（提出+棄却）に占める提出の割合。窓 5 秒と累計の両方を出す。
  // paint あたりの受理: paint 1 枚が何件の提出に結びついたか（1 を超えないことが期待値）。
  const dRej = dUnk + dPose + dSpace + dExp;
  const dDecided = dSub + dRej;
  const winRate = dDecided > 0 ? `${(dSub / dDecided * 100).toFixed(1)}%` : '−';
  const totDecided = vrMeter.submits + vrMeter.unknownId + vrMeter.poseMismatch +
    vrMeter.spaceMismatch + vrMeter.expired;
  const totRate = totDecided > 0 ? `${(vrMeter.submits / totDecided * 100).toFixed(1)}%` : '−';
  const perPaint = dPaints > 0 ? (dSub / dPaints).toFixed(2) : '−';
  log(`[M5 VR計測] paint=${dPaints}/5s（${(dPaints / 5).toFixed(1)}/s） ` +
    `offers=${stats.offers} dones=${stats.dones} stalePaint+${dStale}（累計${stats.droppedStale}） ` +
    `提出+${dSub}（累計${vrMeter.submits}） ` +
    `棄却+${dRej}` +
    `（不明ID+${dUnk} pose不一致+${dPose} 世代不一致+${dSpace} 失効+${dExp}） ` +
    `受理率=${winRate}（窓5s）/${totRate}（累計） paintあたり受理=${perPaint}`);
}
function startVrMeter(): void {
  if (vrMeterTimer !== null) return;
  vrMeter.lastPaints = paintTimes.length;
  vrMeterTimer = setInterval(logVrMeter, 5000);
}
function stopVrMeter(): void {
  if (vrMeterTimer !== null) {
    clearInterval(vrMeterTimer);
    vrMeterTimer = null;
  }
}
// 終了コード確定の状態。パイプ close で finish を確定し、後の exe 非ゼロを無視する競合をなくす。
let exeCode: number | null = null; // exe の終了コード（signal 死は非ゼロ扱い）。未終了は null。
let pipeClosed = false;
let exeWaitTimer: NodeJS.Timeout | null = null;
const EXE_WAIT_MS = 5000; // パイプ close 後の exe 終了待ち上限。
function exeBaseCode(): number { return exeCode === 0 ? 0 : 2; }

let releaseAttempts = 0;
function doReleaseTexture(texture: SharedTexture): void {
  releaseAttempts++;
  texture.release();
}

function releaseOnce(borrowId: number, why: string): boolean {
  const tex = held.get(borrowId);
  if (tex === undefined) return false;
  try {
    doReleaseTexture(tex);
  } catch (e) {
    // 台帳から先に消さない。参照を quarantined に残し、世代終了扱いにする。
    held.delete(borrowId);
    quarantined.set(borrowId, tex);
    stats.quarantined++;
    stopping = true;
    log(`release FAILED borrow=${borrowId} why=${why}: ${e instanceof Error ? e.message : String(e)}（隔離。世代終了扱い）`);
    return false;
  }
  held.delete(borrowId);
  return true;
}

// 借用登録前（台帳に載る前）の texture を返す共通処理。release() 自体が失敗したら
// 参照を捨てず隔離し、世代終了扱いにする（握り潰さない）。
function releaseUnregistered(texture: SharedTexture, why: string): boolean {
  try {
    doReleaseTexture(texture);
  } catch (e) {
    quarantined.set(-quarantined.size - 1, texture);
    stats.quarantined++;
    stopping = true;
    log(`release(登録前) FAILED why=${why}: ${e instanceof Error ? e.message : String(e)}（隔離。世代終了扱い）`);
    return false;
  }
  return true;
}

// paint ハンドラの外側 catch 用。onPaint が登録・返却のどちらも済ませずに投げた
// 借用があれば、共通処理で返却する。台帳管理下のものは触らない。
function releaseOrphanFromPaint(details: unknown, where: string): void {
  const tex = asSharedTexture(
    details !== null && typeof details === 'object'
      ? (details as Record<string, unknown>).texture
      : undefined,
  );
  if (!tex) return;
  for (const v of held.values()) if (v === tex) return;
  for (const v of quarantined.values()) if (v === tex) return;
  log(`paint 外側 catch から未登録借用を返却 (${where})`);
  releaseUnregistered(tex, where);
}

function noteHeld(): void {
  if (held.size > stats.maxHeld) stats.maxHeld = held.size;
}

// GPU プロセスの pid を app.getAppMetrics() から取る。取れなければ null。
function currentGpuPid(): number | null {
  try {
    const metrics = app.getAppMetrics();
    const gpu = metrics.find((m) => String(m && m.type).toLowerCase() === 'gpu');
    if (gpu && typeof gpu.pid === 'number') return gpu.pid;
    return null;
  } catch (e) {
    log(`getAppMetrics 失敗: ${e instanceof Error ? e.message : String(e)}`);
    return null;
  }
}

function makeOfferToExe(borrowId: number, width: number, height: number, ntHandle: Buffer): boolean {
  if (addon === null || exeProc === null || exeProc.pid === undefined) {
    stats.offerFailures++;
    log(`offer 不能 borrow=${borrowId}（addon/exe が無い）`);
    releaseOnce(borrowId, 'no-addon-or-exe');
    return false;
  }
  // 複製して送る。失敗したらその借用だけ release して失敗扱いにする。
  let dup: { ok: boolean; handle?: bigint };
  try {
    dup = addon.duplicateHandle(Buffer.from(ntHandle), exeProc.pid);
  } catch (e) {
    stats.offerFailures++;
    log(`duplicateHandle THROW borrow=${borrowId}: ${e instanceof Error ? e.message : String(e)}`);
    releaseOnce(borrowId, 'dup-throw');
    return false;
  }
  if (!dup || dup.ok !== true || typeof dup.handle !== 'bigint') {
    stats.offerFailures++;
    log(`duplicateHandle FAILED borrow=${borrowId}: ${JSON.stringify(dup)}`);
    releaseOnce(borrowId, 'dup-failed');
    return false;
  }
  try {
    socket?.write(makeOffer(borrowId, width, height, dup.handle));
  } catch (e) {
    stats.offerFailures++;
    log(`offer send FAILED borrow=${borrowId}: ${e instanceof Error ? e.message : String(e)}`);
    releaseOnce(borrowId, 'send-failed');
    return false;
  }
  stats.offers++;
  return true;
}

function onPaint(details: unknown): void {
  paintTimes.push(Date.now());
  // 初回だけ paint の形状を記録する（版差の観測用。以後は出さない）。
  if (!paintShapeLogged) {
    paintShapeLogged = true;
    log(`paint 形状: ${describePaint(details)}`);
  }
  const texture = asSharedTexture(
    details !== null && typeof details === 'object'
      ? (details as Record<string, unknown>).texture
      : undefined,
  );
  if (!texture) {
    stats.noTexture++;
    return;
  }
  const ti = texture.textureInfo;
  const nt = ti.handle.ntHandle;
  const coded = ti.codedSize;
  if (stopping) {
    // 計測終了後。借用として登録せず共通処理で返す（握り潰さない）。
    stats.drops++;
    releaseUnregistered(texture, 'stopping');
    return;
  }
  if (ti.pixelFormat !== 'bgra' || !Buffer.isBuffer(nt) || nt.length !== 8) {
    if (ti.pixelFormat !== 'bgra') stats.formatMismatch++;
    else stats.missingHandle++;
    releaseUnregistered(texture, 'format-mismatch');
    return;
  }
  // ADR: xrActive=false（ブラウジング中）の paint は Offer しない。VR 開始前の
  // プレースホルダ画像を送ると exe 側で不明IDの棄却になり無駄な転送になるため、
  // texture を即 release して捨て、件数だけ数える（以後の受理率の切り分け用）。
  if (!xrActive) {
    stats.droppedBrowsing++;
    releaseUnregistered(texture, 'browsing-no-offer');
    return;
  }
  // ADR: ページの初回描画までは Offer しない。記録配送直後の paint にはまだ
  // 旧内容しか載らず、送ると不明IDの棄却になる。シムの初回スタンプ通知
  // （`xr:stamped`。無い橋・描かないページ用に 2 秒の予備期限つき）で開ける。
  if (!vrPixelsValid) {
    stats.droppedBrowsing++;
    releaseUnregistered(texture, 'waiting-first-stamp');
    return;
  }
  if (stats.firstCodedSize === null) {
    stats.firstCodedSize = { width: coded.width, height: coded.height };
    log(`first codedSize=${coded.width}x${coded.height} pixelFormat=${ti.pixelFormat}`);
  } else if (stats.firstCodedSize.width !== coded.width || stats.firstCodedSize.height !== coded.height) {
    // XR 開始でキャンバスが広がるため、寸法変化は 1 行だけ記録する（毎 paint は出さない）
    log(`codedSize 変化: ${stats.firstCodedSize.width}x${stats.firstCodedSize.height} → ${coded.width}x${coded.height}`);
    stats.firstCodedSize = { width: coded.width, height: coded.height };
  }
  const hex = (nt as Buffer).toString('hex');
  handleCounts.set(hex, (handleCounts.get(hex) || 0) + 1);

  // フロー制御: 未処理の Offer（inFlight）がある間の paint は古いため、
  // duplicate/offer せず texture を即 release して捨てる。未処理の record に
  // 対応しない paint であり、既存の drops と区別して droppedStale に数える。
  // --pace-records の exe と対になり、発行と消費を 1:1 に保つ。
  if (inFlight !== null) {
    stats.droppedStale++;
    releaseUnregistered(texture, 'stale-superseded');
    return;
  }

  // 借用登録（release の所有権を台帳が持つ）。
  const borrowId = nextBorrowId++;
  held.set(borrowId, texture);
  noteHeld();

  if (socket === null || socket.destroyed) {
    // exe 未接続。即返す（転送前 drop）。
    stats.drops++;
    releaseOnce(borrowId, 'not-connected');
    return;
  }
  if (makeOfferToExe(borrowId, coded.width, coded.height, Buffer.from(nt as Buffer))) {
    inFlight = borrowId;
  }
  // makeOfferToExe の失敗時は内部で release 済みのため何もしない。
}
let paintShapeLogged = false;

/** 版差の観測用に paint 引数の形状を 1 行で表す。借用には触らない。 */
function describePaint(details: unknown): string {
  if (details === null || typeof details !== 'object') return `type=${typeof details}`;
  const rec = details as Record<string, unknown>;
  const keys = Object.keys(rec).slice(0, 12).join(',');
  const tex = rec.texture;
  if (tex === null || typeof tex !== 'object') return `keys=[${keys}] texture=${typeof tex}`;
  const t = tex as Record<string, unknown>;
  const ti = t.textureInfo;
  const tiKeys = ti !== null && typeof ti === 'object' ? Object.keys(ti as Record<string, unknown>).slice(0, 8).join(',') : typeof ti;
  return `keys=[${keys}] textureKeys=[${Object.keys(t).slice(0, 8).join(',')}] textureInfo=[${tiKeys}]`;
}

function onDone(borrowId: number, status: number): void {
  if (borrowId === inFlight) {
    inFlight = null;
    stats.dones++;
    if (status === STATUS_OK) stats.statusOk++;
    else if (status === 2) stats.statusVerifyFailed++;
    else stats.statusCopyFailed++;
    releaseOnce(borrowId, 'done');
    maybeFinish();
  } else {
    // 重複・遅延 ACK は無視する（台帳の直列化）。
    stats.staleDones++;
    log(`stale Done を無視 borrow=${borrowId} status=${status}`);
  }
}

// exe からの RenderRequest を renderer へ渡す。pose/FOV は描画要求であり、
// main プロセスは値を推測・付与しない。使用証拠は画素内の ID のみである。
// OSR ウィンドウの content サイズは要求（眼ごと reqWidth/reqHeight）に追従させる。
// 両眼横並びのため 2*reqWidth × reqHeight にする。初回とサイズ変更時のみ変える。
// VR への切替は xrActive（renderer の requestSession 通知）で決める。
// xrActive が false の間はリサイズも転送もしない（サイトを開いただけでは VR にしない）。
// self-test の VR リサイズ検証もこの関数を通す（本番と同一経路）。
function applyVrResize(reqW: number, reqH: number, renderId: string): void {
  const osr = winRef;
  if (osr === null || osr.isDestroyed()) return;
  const targetW = reqW * 2;
  const targetH = reqH;
  const [curW, curH] = osr.getContentSize();
  if (lastReqSize === null || lastReqSize.w !== targetW || lastReqSize.h !== targetH ||
      curW !== targetW || curH !== targetH) {
    try {
      osr.setContentSize(targetW, targetH);
      log(`window リサイズ: ${curW}x${curH} → ${targetW}x${targetH}（req ${reqW}x${reqH}/眼 renderId=${renderId}）`);
    } catch (e) {
      log(`window リサイズ失敗 ${targetW}x${targetH}: ${e instanceof Error ? e.message : String(e)}`);
    }
    lastReqSize = { w: targetW, h: targetH };
  }
  if (!vrActive) {
    vrActive = true;
    if (MIRROR_VR_FPS <= 0) {
      log(`ミラー停止（VR 開始 renderId=${renderId}。既定 0 のため VR 中は止める）`);
    } else {
      log(`ミラー節約に切替: ${MIRROR_FPS}fps → ${MIRROR_VR_FPS}fps（VR 開始 renderId=${renderId}。VR の負荷を優先）`);
    }
    restartMirrorTimer();
  }
}
let lastReqSize: { w: number; h: number } | null = null;
let recordLogCount = 0;
// ADR: xrActive が立つ前に届いた要求は数えるだけであり、保持した最新も
// VR 開始時に破棄する。古いレコードを転送すると exe 側で TTL 切れ・不明IDに
// なり、次の新規レコードまで詰まる（実機で約 23 秒の全棄却を確認）。
// requestSession の解決は有効化後の新規到着で行う。exe は pace の停滞
// タイムアウト（500ms）で新規発行するため deadlock しない。
// 終了通知でも破棄する（古い要求を次の開始時に持ち越さない）。
let bufferedRecord: XrRecord | null = null;
// VR 開始時刻（Date.now。最初の記録・受理までの時間計測用）。
let xrActiveSince: number | null = null;
// 有効化後に最初に配送した時刻。未配送は null。
let vrFirstRecordAt: number | null = null;
// ページが初回の制御ブロックを描いたか。立つまで Offer せず旧内容を送らない。
// シムの `stamped` 通知で立つ（来ない橋・描かないページ用の予備期限つき）。
let vrPixelsValid = false;
// 有効化後の最初の受理ログを出したか（セッションごとに立て直す）。
let vrFirstAcceptLogged = false;
// 有効化後 1 秒の警告タイマー。新規レコードが来なければ pace 停滞を疑う。
let vrNoRecordWarnTimer: NodeJS.Timeout | null = null;
// 初回描画の予備期限（2 秒）。描かないページでも XR を空回りさせない。
// 通常の描画遅延（実測 350〜500ms）より十分長く、固まり放置より短い仮の値である。
let vrFirstStampTimer: NodeJS.Timeout | null = null;
// 復号済みレコードの配送（リサイズ＋転送）。xrActive が真のときだけ呼ぶ。
function deliverRecord(rec: XrRecord, via: string): void {
  if (rec.reqWidth > 0 && rec.reqHeight > 0 &&
      winRef !== null && !winRef.isDestroyed()) {
    applyVrResize(rec.reqWidth, rec.reqHeight, rec.renderId);
    // 最初の数回だけ、要求と窓の対応を記録する（以後の canvas 側は renderer が出す）。
    // ADR: 条件式で null・破棄済みを弾いているため、ここでは絞り込みが残る。
    // 間に挟まるのは読み取り専用の参照渡しだけであり、winRef への代入は無い。
    if (recordLogCount < 3 && winRef !== null && !winRef.isDestroyed()) {
      recordLogCount++;
      const [nowW, nowH] = winRef.getContentSize();
      log(`[M5記録] #${recordLogCount} renderId=${rec.renderId} req=${rec.reqWidth}x${rec.reqHeight}/眼 window=${nowW}x${nowH} xrActive=true${via}`);
    }
    if (winRef !== null && !winRef.isDestroyed()) winRef.webContents.send('xr:record', rec);
    if (xrActive && vrFirstRecordAt === null && xrActiveSince !== null) {
      vrFirstRecordAt = Date.now();
      if (vrNoRecordWarnTimer !== null) {
        clearTimeout(vrNoRecordWarnTimer);
        vrNoRecordWarnTimer = null;
      }
      log(`[M5 VR開始] 最初の記録まで ${vrFirstRecordAt - xrActiveSince}ms（renderId=${rec.renderId}）`);
    }
  } else if (winRef !== null && !winRef.isDestroyed()) {
    winRef.webContents.send('xr:record', rec);
  }
}
function forwardRecord(m: Buffer): void {
  const rec = decodeRecord(m);
  if (rec === null) {
    log('RenderRequest の復号に失敗（配置不正）');
    return;
  }
  stats.renderRequests++;
  // ADR: xrActive が false の間は VR サイズへ広げず renderer へも転送しない。
  // exe は起動時から送り続けるため、サイトを開いただけではブラウジング寸法のまま保つ。
  // 件数は数え、最新 1 件の ID だけ残す（VR 開始時に破棄した旨と合わせて記録するため）。
  if (!xrActive) {
    xrDroppedRecords++;
    bufferedRecord = rec;
    if (xrDroppedRecords <= 3 || xrDroppedRecords % 500 === 0) {
      const sizeNote = winRef !== null && !winRef.isDestroyed()
        ? ` window=${winRef.getContentSize().join('x')} のまま`
        : '';
      log(`RenderRequest を保留（xrActive=false のため転送せず最新 1 件を保持 renderId=${rec.renderId} 累計${xrDroppedRecords}件${sizeNote}）`);
    }
    return;
  }
  deliverRecord(rec, '');
}

// renderer のセッション開始・終了の通知で xrActive を切り替える。
// 開始時は保留分を破棄して以後の新規レコードから転送し、終了時はブラウジングに戻す。
function setXrActive(active: boolean): void {
  if (xrActive === active) return;
  xrActive = active;
  log(`xrActive=${active ? 'true（VR 開始。保留分は破棄し新規レコードから転送する）' : 'false（セッション終了。転送を止める）'}`);
  if (active) {
    if (!vrActive) {
      vrActive = true;
      if (MIRROR_VR_FPS <= 0) {
        log(`ミラー停止（VR 開始。既定 0 のため VR 中は止める）`);
      } else {
        log(`ミラー節約に切替: ${MIRROR_FPS}fps → ${MIRROR_VR_FPS}fps（VR 開始。VR の負荷を優先）`);
      }
      restartMirrorTimer();
    }
    // 通知前に届いた保留分は古いため転送せず破棄する。以後の新規到着から始める。
    // exe は pace の停滞タイムアウト（500ms）で新規発行するため 1 秒以内に来るはずである。
    if (bufferedRecord !== null) {
      const rec = bufferedRecord;
      bufferedRecord = null;
      log(`保留中の RenderRequest を破棄（renderId=${rec.renderId}。VR開始は新規レコードから始める。累計保留${xrDroppedRecords}件）`);
    }
    xrActiveSince = Date.now();
    vrFirstRecordAt = null;
    vrPixelsValid = false;
    vrFirstAcceptLogged = false;
    if (vrNoRecordWarnTimer !== null) {
      clearTimeout(vrNoRecordWarnTimer);
      vrNoRecordWarnTimer = null;
    }
    if (vrFirstStampTimer !== null) {
      clearTimeout(vrFirstStampTimer);
      vrFirstStampTimer = null;
    }
    // 動作は変えない。新規レコードが 1 秒待っても来ない場合だけ警告する。
    // exe 無し動作（--no-exe・--self-test）では記録が来ないため予約しない。
    if (!NO_EXE && !SELF_TEST) {
      vrNoRecordWarnTimer = setTimeout(() => {
        vrNoRecordWarnTimer = null;
        if (xrActive && vrFirstRecordAt === null) {
          log('警告: VR開始から1秒経っても RenderRequest が来ない（exe の pace 停滞タイムアウトを確認）');
        }
      }, 1000);
      // 描かないページでも空回りさせるための予備期限。2 秒で開ける。
      vrFirstStampTimer = setTimeout(() => {
        vrFirstStampTimer = null;
        if (xrActive && !vrPixelsValid) {
          vrPixelsValid = true;
          log('警告: VR開始から2秒経っても初回描画の通知が無いため Offer を開始する（ページが描いていない可能性）');
        }
      }, 2000);
    }
  } else {
    bufferedRecord = null;
    xrActiveSince = null;
    vrFirstRecordAt = null;
    vrPixelsValid = false;
    if (vrNoRecordWarnTimer !== null) {
      clearTimeout(vrNoRecordWarnTimer);
      vrNoRecordWarnTimer = null;
    }
    if (vrFirstStampTimer !== null) {
      clearTimeout(vrFirstStampTimer);
      vrFirstStampTimer = null;
    }
    if (vrActive) {
      vrActive = false;
      log(`ミラー復帰: ブラウジング ${MIRROR_FPS}fps に戻す（セッション終了）`);
      restartMirrorTimer();
    }
  }
}

// exe からの InputState を renderer へ渡す。1 秒に 1 回だけ要約をログに出す。
let lastInputLogAt = 0;
function onInputState(m: Buffer): void {
  const snap = decodeInput(m);
  if (snap === null) {
    log('InputState の復号に失敗（配置不正）');
    return;
  }
  stats.inputStates++;
  stats.lastInput = snap;
  if (winRef !== null && !winRef.isDestroyed()) {
    winRef.webContents.send('xr:input', snap);
  }
  const now = Date.now();
  if (now - lastInputLogAt >= 1000) {
    lastInputLogAt = now;
    log(`[M5入力] L: ${summarizeHand(snap.left)} | R: ${summarizeHand(snap.right)} ` +
      `(frame=${snap.frame} conn=${snap.connEpoch} space=${snap.spaceEpoch} ` +
      `latch=L:${snap.latchLeft} R:${snap.latchRight})`);
  }
}

function summarizeHand(h: InputSnapshot['left']): string {
  return `${h.connected ? '接続' : '未接続'} prof=${h.profile} ` +
    `trig=${h.trigger.toFixed(2)} sq=${h.squeeze.toFixed(2)} ` +
    `stick=(${h.thumbstick[0].toFixed(2)},${h.thumbstick[1].toFixed(2)}) ` +
    `grip=${h.grip.valid ? '有効' : '無効'} aim=${h.aim.valid ? '有効' : '無効'}`;
}

// HapticCommand を exe へ送る。送信結果（成功・inactive・失敗）は exe 側のログと統計に残る。
function sendHaptic(hand: number, amplitude: number, durationMs: number): void {
  if (socket === null || socket.destroyed) {
    log('haptics 未送信: パイプが無い');
    return;
  }
  try {
    socket.write(makeHaptic(hand, amplitude, durationMs));
    stats.hapticSent++;
    log(`haptics 指令を送信（${hand === 0 ? '左' : '右'}・振幅 ${amplitude}・${durationMs}ms。inactive・失敗は exe 側の統計に残る）`);
  } catch (e) {
    log(`haptics 送信に失敗: ${e instanceof Error ? e.message : String(e)}`);
  }
}

// 検証専用の画面 capture（--capture-at=Ms,path）。指定時刻に
// webContents.capturePage() で PNG を保存する。描画経路には入れない。
function scheduleCapture(win: BrowserWindow): void {
  for (const c of CAPTURE_AT_LIST) {
    log(`検証フック有効: --capture-at=${c.ms},${c.file}（起動からその ms 後に capturePage で PNG 保存）`);
    captureTimers.push(setTimeout(() => {
      void (async (): Promise<void> => {
        try {
          if (win.isDestroyed()) {
            log(`capture スキップ（window 破棄済み）: ${c.file}`);
            return;
          }
          const img = await win.webContents.capturePage();
          const out = path.isAbsolute(c.file) ? c.file : path.join(logDir, c.file);
          const dir = path.dirname(out);
          if (dir !== '' && !fs.existsSync(dir)) fs.mkdirSync(dir, { recursive: true });
          fs.writeFileSync(out, img.toPNG());
          stats.captures++;
          log(`capture 保存: ${out}（${c.ms}ms 時点）`);
        } catch (e) {
          log(`capture 失敗 ${c.file}: ${e instanceof Error ? e.message : String(e)}`);
        }
      })();
    }, c.ms));
  }
}

/**
 * C 案のミラー。OSR（不可視）の capturePage をタイマーで拾い、Bitmap を
 * コントロール窓へ送る。VR 中は既定で止めて VR の負荷を優先する。
 * 解像度は OSR の content サイズそのままであり、黒帯つきの等倍表示は
 * コントロール側（pages/control.html）が担う。
 */

// 現在のミラー間隔（ms）。VR 中は節約間隔に切り替える。0 以下は停止の合図である。
function currentMirrorInterval(): number {
  const fps = vrActive ? MIRROR_VR_FPS : MIRROR_FPS;
  if (!(fps > 0)) return 0;
  return Math.round(1000 / fps);
}

// VR 中のミラー停止中か。間隔 0 のとき真である。
function isMirrorPausedForVr(): boolean {
  return vrActive && !(MIRROR_VR_FPS > 0);
}

// capturePage の結果から送信用の Bitmap を取り出す。
// 版差に備え、形状は実行時に確認する（型注釈だけでは保証できない）。
function extractMirrorBitmap(img: NativeImage): { width: number; height: number; data: Buffer } | null {
  const size = img.getSize();
  if (!(size.width > 0) || !(size.height > 0)) return null;
  let raw: Buffer;
  try {
    raw = img.toBitmap();
  } catch (e) {
    log(`mirror toBitmap 失敗: ${e instanceof Error ? e.message : String(e)}`);
    return null;
  }
  if (!Buffer.isBuffer(raw)) return null;
  if (raw.length < size.width * size.height * 4) {
    log(`mirror bitmap 不足: ${raw.length}B に対し ${size.width}x${size.height}x4 を期待`);
    return null;
  }
  return { width: size.width, height: size.height, data: Buffer.from(raw) };
}

// ミラーの 1 拍。重なりを避けるため setTimeout 鎖で回す（setInterval にしない）。
async function mirrorTick(): Promise<void> {
  mirrorTimer = null;
  const osr = winRef;
  const ctl = controlRef;
  if (mirrorStopped || osr === null || osr.isDestroyed() || ctl === null || ctl.isDestroyed()) return;
  const t0 = performance.now();
  try {
    const img = await osr.webContents.capturePage();
    const bmp = extractMirrorBitmap(img);
    const spent = performance.now() - t0;
    if (bmp === null) {
      stats.mirrorErrors++;
    } else {
      mirrorTotalMs += spent;
      if (spent > stats.mirrorCaptureMaxMs) stats.mirrorCaptureMaxMs = spent;
      stats.mirrorFrames++;
      mirrorTimes.push(Date.now());
      if (mirrorTimes.length > 100) mirrorTimes.splice(0, mirrorTimes.length - 100);
      if (!ctl.isDestroyed()) {
        ctl.webContents.send('mirror:frame', { width: bmp.width, height: bmp.height, data: bmp.data });
      }
      if (stats.mirrorFrames <= 3 || stats.mirrorFrames % 100 === 0) {
        log(`mirror frame #${stats.mirrorFrames} ${bmp.width}x${bmp.height} ` +
          `capture=${spent.toFixed(1)}ms mode=${vrActive ? `vr(${MIRROR_VR_FPS}fps)` : `browse(${MIRROR_FPS}fps)`}`);
      }
    }
  } catch (e) {
    stats.mirrorErrors++;
    log(`mirror capture 失敗: ${e instanceof Error ? e.message : String(e)}`);
  }
  if (!mirrorStopped) scheduleMirrorTick();
}

// 次拍を固定レートで予約する。1 拍の所要が間隔を超えた分は詰め、
// 溜まった遅れは捨てる（最新優先。借用台帳と同じ考え方）。
// VR 中の停止時（間隔 0）は予約しない。
function scheduleMirrorTick(): void {
  if (mirrorStopped || isMirrorPausedForVr()) return;
  const interval = currentMirrorInterval();
  if (!(interval > 0)) return;
  mirrorNextAt += interval;
  const now = Date.now();
  // 1 拍分以上遅れたら backlog を捨てて今から取り直す（追いつき連打にしない）。
  if (mirrorNextAt < now - interval) mirrorNextAt = now + interval;
  mirrorTimer = setTimeout(() => { void mirrorTick(); }, Math.max(0, mirrorNextAt - now));
}

// ミラー開始。モード切替時（VR 開始）は restartMirrorTimer で間隔を変える。
function startMirror(): void {
  if (mirrorTimer !== null || mirrorStopped) return;
  if (isMirrorPausedForVr()) {
    log(`mirror 開始を保留（VR 中のため停止。ブラウジングに戻れば ${MIRROR_FPS}fps で再開）`);
    return;
  }
  const interval = currentMirrorInterval();
  log(`mirror 開始: ${MIRROR_FPS}fps（VR 中は ${MIRROR_VR_FPS <= 0 ? '停止' : `${MIRROR_VR_FPS}fps に節約`}）`);
  mirrorNextAt = Date.now() + interval;
  mirrorTimer = setTimeout(() => { void mirrorTick(); }, interval);
}

// 節約間隔への切替用。停止後は再開しない。VR 中の停止時はタイマーを解除するだけである。
function restartMirrorTimer(): void {
  if (mirrorStopped) return;
  if (mirrorTimer !== null) {
    clearTimeout(mirrorTimer);
    mirrorTimer = null;
  }
  if (isMirrorPausedForVr()) return;
  const interval = currentMirrorInterval();
  if (!(interval > 0)) return;
  mirrorNextAt = Date.now() + interval;
  mirrorTimer = setTimeout(() => { void mirrorTick(); }, interval);
}

// ミラー停止。タイマー解除だけであり、送り済みフレームの後始末は要らない。
// 終了処理・self-test の停止確認から呼ぶ（安全な多重呼び出し）。
function stopMirror(): void {
  mirrorStopped = true;
  if (mirrorTimer !== null) {
    clearTimeout(mirrorTimer);
    mirrorTimer = null;
  }
}

// 直近の paint fps（5 秒窓）。ミラー fps（10 秒窓）と合わせて状態行に載せる。
function recentFps(times: readonly number[], windowMs: number): number {
  const now = Date.now();
  let n = 0;
  for (let i = times.length - 1; i >= 0; i--) {
    if (now - times[i] > windowMs) break;
    n++;
  }
  return (n * 1000) / windowMs;
}

function broadcastMirrorStatus(): void {
  const ctl = controlRef;
  if (ctl === null || ctl.isDestroyed()) return;
  const osr = winRef;
  let osrW = 0, osrH = 0;
  if (osr !== null && !osr.isDestroyed()) {
    [osrW, osrH] = osr.getContentSize();
  }
  ctl.webContents.send('mirror:status', {
    connected: socket !== null && !socket.destroyed,
    vr: vrActive,
    fps: recentFps(paintTimes, 5000),
    mirrorFps: recentFps(mirrorTimes, 10000),
    captureMsAvg: stats.mirrorFrames > 0 ? mirrorTotalMs / stats.mirrorFrames : 0,
    osrW, osrH,
  });
}

function startStatusBroadcast(): void {
  if (statusTimer !== null) return;
  statusTimer = setInterval(broadcastMirrorStatus, 1000);
}

// コントロール窓からの入力を OSR へ転送する。境界検証は mirror-input が担い、
// ここでは OSR の生存確認・フォーカス・送信と件数の記録だけを行う。
function handleMirrorInput(raw: unknown): void {
  const osr = winRef;
  if (osr === null || osr.isDestroyed()) {
    stats.inputErrors++;
    return;
  }
  const [osrW, osrH] = osr.getContentSize();
  const ev = toOsrInputEvent(raw, osrW, osrH);
  if (ev === null) {
    stats.inputErrors++;
    return;
  }
  try {
    // ADR: sendInputEvent は対象 webContents の内部フォーカスを要するため、
    // 転送の直前に focus する（OS のフォーカスはコントロール窓に残る）。
    osr.webContents.focus();
    osr.webContents.sendInputEvent(ev);
    stats.inputForwarded++;
  } catch (e) {
    stats.inputErrors++;
    log(`mirror 入力の送信に失敗: ${e instanceof Error ? e.message : String(e)}`);
  }
}

async function createControlWindow(): Promise<BrowserWindow> {
  const win = new BrowserWindow({
    width: CTL_W,
    height: CTL_H,
    show: true,
    autoHideMenuBar: true,
    title: 'XRift control（OSR ミラー）',
    webPreferences: {
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
      preload: path.join(__dirname, 'control-preload.js'),
      backgroundThrottling: false,
    },
  });
  controlRef = win;
  log(`control window created: ${CTL_W}x${CTL_H}（可視・ミラー表示 + 入力受付）`);
  win.on('focus', () => {
    // コントロール窓が触られたら OSR の内部フォーカスを戻す（キー転送用）。
    const osr = winRef;
    if (osr !== null && !osr.isDestroyed()) {
      try { osr.webContents.focus(); } catch { /* 終了時は無視 */ }
    }
  });
  win.on('closed', () => {
    if (controlRef === win) controlRef = null;
    // ADR: コントロール窓は可視の唯一の操作対象である。閉じたらアプリ全体を
    // 終了する（OSR 窓は不可視のため閉じる操作の対象にならない）。
    // 処理中の offer を隔離しないよう優雅な停止に入る。finish の finished 旗で
    // 多重呼び出しを防ぐため、close 競合でハングしない。
    log('control window closed（アプリ全体を終了する）');
    beginShutdown(0);
  });
  win.webContents.on('did-finish-load', () => {
    log('control did-finish-load');
  });
  win.webContents.on('did-fail-load', (_event, code, desc, validatedUrl) => {
    log(`control did-fail-load code=${code} desc=${desc} url=${validatedUrl}`);
  });
  await win.loadFile(path.join(__dirname, '..', 'pages', 'control.html'));
  return win;
}

// 検証専用のコントロール窓 capture（--control-capture-at=Ms,path）。
function scheduleControlCapture(win: BrowserWindow): void {
  for (const c of CONTROL_CAPTURE_AT_LIST) {
    log(`検証フック有効: --control-capture-at=${c.ms},${c.file}`);
    controlCaptureTimers.push(setTimeout(() => {
      void (async (): Promise<void> => {
        try {
          if (win.isDestroyed()) {
            log(`control capture スキップ（window 破棄済み）: ${c.file}`);
            return;
          }
          const img = await win.webContents.capturePage();
          const out = path.isAbsolute(c.file) ? c.file : path.join(logDir, c.file);
          const dir = path.dirname(out);
          if (dir !== '' && !fs.existsSync(dir)) fs.mkdirSync(dir, { recursive: true });
          fs.writeFileSync(out, img.toPNG());
          stats.controlCaptures++;
          log(`control capture 保存: ${out}（${c.ms}ms 時点）`);
        } catch (e) {
          log(`control capture 失敗 ${c.file}: ${e instanceof Error ? e.message : String(e)}`);
        }
      })();
    }, c.ms));
  }
}

function sleepMs(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

// self-test（HMD 不要）。ミラー・入力転送・VR リサイズ・停止を本番経路で確かめる。
// OSR には pages/mirror-test.html を開き、ボタン押下と文字入力を handleMirrorInput
// 経由（= コントロール窓と同じ入口）で送り、DOM の変化と PNG で確認する。
async function runSelfTest(osr: BrowserWindow, control: BrowserWindow): Promise<void> {
  const failures: string[] = [];
  const pass = (m: string): void => { log(`[SELFTEST] PASS ${m}`); };
  const fail = (m: string): void => { log(`[SELFTEST] FAIL ${m}`); failures.push(m); };
  log('[SELFTEST] 開始（mirror-test.html で検証する）');
  try {
    await sleepMs(1500);
    if (osr.isDestroyed() || control.isDestroyed()) {
      fail('window が検証前に破棄された');
      finish(2);
      return;
    }
    // ボタンの押下。コントロール窓と同じ入口（handleMirrorInput）を通す。
    const rect = await osr.webContents.executeJavaScript(
      `(() => { const r = document.getElementById('btn').getBoundingClientRect(); ` +
      `return { x: r.x, y: r.y, width: r.width, height: r.height }; })()`,
    ) as unknown;
    if (rect === null || typeof rect !== 'object') {
      fail('ボタンの矩形が取れない');
    } else {
      const r = rect as Record<string, unknown>;
      const cx = (r.x as number) + (r.width as number) / 2;
      const cy = (r.y as number) + (r.height as number) / 2;
      handleMirrorInput({ kind: 'mouseMove', x: cx, y: cy });
      await sleepMs(150);
      handleMirrorInput({ kind: 'mouseDown', x: cx, y: cy, button: 'left', clickCount: 1 });
      await sleepMs(100);
      handleMirrorInput({ kind: 'mouseUp', x: cx, y: cy, button: 'left', clickCount: 1 });
      await sleepMs(600);
      const text = await osr.webContents.executeJavaScript(
        `document.getElementById('result').textContent`,
      ) as unknown;
      if (typeof text === 'string' && text.includes('1回')) pass(`ボタン押下の転送 result=${text}`);
      else fail(`ボタン押下が反映しない result=${String(text)}`);
    }
    // 文字入力。入力欄を押してから char を送る（ログイン等の等価確認）。
    const nameRect = await osr.webContents.executeJavaScript(
      `(() => { const r = document.getElementById('name').getBoundingClientRect(); ` +
      `return { x: r.x, y: r.y, width: r.width, height: r.height }; })()`,
    ) as unknown;
    if (nameRect === null || typeof nameRect !== 'object') {
      fail('入力欄の矩形が取れない');
    } else {
      const r = nameRect as Record<string, unknown>;
      const cx = (r.x as number) + (r.width as number) / 2;
      const cy = (r.y as number) + (r.height as number) / 2;
      handleMirrorInput({ kind: 'mouseDown', x: cx, y: cy, button: 'left', clickCount: 1 });
      await sleepMs(100);
      handleMirrorInput({ kind: 'mouseUp', x: cx, y: cy, button: 'left', clickCount: 1 });
      await sleepMs(200);
      for (const ch of ['a', 'b']) {
        handleMirrorInput({ kind: 'keyDown', keyCode: ch, modifiers: [] });
        handleMirrorInput({ kind: 'char', keyCode: ch });
        handleMirrorInput({ kind: 'keyUp', keyCode: ch, modifiers: [] });
        await sleepMs(120);
      }
      const value = await osr.webContents.executeJavaScript(
        `(document.getElementById('name')).value`,
      ) as unknown;
      if (value === 'ab') pass('文字入力の転送 value=ab');
      else fail(`文字入力が反映しない value=${String(value)}`);
    }
    // ミラーが回っていること（画像が届いていること）。
    if (stats.mirrorFrames > 0) {
      const avg = mirrorTotalMs / stats.mirrorFrames;
      pass(`ミラー送信 frames=${stats.mirrorFrames} capture平均=${avg.toFixed(1)}ms`);
      log(`[SELFTEST] capturePage 平均=${avg.toFixed(1)}ms 最大=${stats.mirrorCaptureMaxMs.toFixed(1)}ms ` +
        `（10fps 周期=100ms に対する余裕の目安）`);
    } else {
      fail('ミラーが 1 枚も送られていない');
    }
    // コントロール窓の capture を PNG に保存し、目視証拠にする。
    try {
      const img = await control.webContents.capturePage();
      const out = path.join(logDir, 'mirror.png');
      fs.writeFileSync(out, img.toPNG());
      stats.controlCaptures++;
      pass(`コントロール窓の保存 ${out}`);
    } catch (e) {
      fail(`コントロール窓の保存に失敗: ${e instanceof Error ? e.message : String(e)}`);
    }
    // VR 開始の等価確認。セッション開始の通知から RenderRequest と同じ関数で
    // OSR を VR サイズへ広げ、ミラーが VR モード（既定は停止）でハングしないことを確かめる。
    const framesBefore = stats.mirrorFrames;
    // ADR: 本番では renderer の requestSession 通知で xrActive が立つため、
    // self-test でも同一の入口（setXrActive）を通してからリサイズする。
    setXrActive(true);
    if (!xrActive) fail('setXrActive(true) で xrActive が立たない');
    else pass('xrActive が立つ（セッション開始の通知）');
    applyVrResize(640, 640, 'selftest');
    await sleepMs(1500);
    if (control.isDestroyed() || osr.isDestroyed()) {
      fail('VR リサイズ後に window が破棄された');
    } else {
      const [vw, vh] = osr.getContentSize();
      if (vw === 1280 && vh === 640) pass(`VR リサイズ window=${vw}x${vh}`);
      else fail(`VR リサイズの寸法が違う window=${vw}x${vh}`);
      if (MIRROR_VR_FPS <= 0) {
        if (mirrorTimer === null) pass(`VR 中ミラー停止（既定 0。frames=${framesBefore}のまま）`);
        else fail('VR 中ミラーが止まっていない（既定 0 なのにタイマーが残る）');
      } else if (stats.mirrorFrames > framesBefore) {
        pass(`節約ミラー継続 frames=${framesBefore}→${stats.mirrorFrames}`);
      } else {
        fail('節約ミラーが進んでいない（ハングの疑い）');
      }
      try {
        const img = await control.webContents.capturePage();
        const out = path.join(logDir, 'mirror-vr.png');
        fs.writeFileSync(out, img.toPNG());
        stats.controlCaptures++;
        pass(`VR 中コントロール窓の保存 ${out}（黒帯つき維持の目視用）`);
      } catch (e) {
        fail(`VR 中コントロール窓の保存に失敗: ${e instanceof Error ? e.message : String(e)}`);
      }
    }
    // セッション終了の等価確認。xrActive を落とすとミラーがブラウジングに戻ること。
    setXrActive(false);
    if (xrActive) fail('setXrActive(false) で xrActive が落ちない');
    else pass('xrActive が落ちる（セッション終了の通知）');
    await sleepMs(600);
    // コントロール窓のリサイズ追従。窓を細くしてから戻し、ミラーが続くことを確かめる。
    // OSR 側のアスペクト追従は VR リサイズで済ませているため、ここでは窓側だけを見る。
    try {
      const [cw, ch] = control.getSize();
      const framesMid = stats.mirrorFrames;
      control.setSize(760, 500);
      await sleepMs(900);
      if (control.isDestroyed()) {
        fail('リサイズでコントロール窓が壊れた');
      } else if (stats.mirrorFrames <= framesMid) {
        fail('リサイズ後にミラーが進んでいない');
      } else {
        const img = await control.webContents.capturePage();
        const out = path.join(logDir, 'mirror-resize.png');
        fs.writeFileSync(out, img.toPNG());
        stats.controlCaptures++;
        pass(`コントロール窓のリサイズ追従 ${cw}x${ch}→760x500（${out}）`);
      }
      if (!control.isDestroyed()) control.setSize(cw, ch);
      await sleepMs(400);
    } catch (e) {
      fail(`リサイズ追従の確認に失敗: ${e instanceof Error ? e.message : String(e)}`);
    }
    // 停止処理の安全確認。タイマー解除後に破棄しても投げないこと。
    stopMirror();
    if (mirrorTimer === null && mirrorStopped) pass('ミラー停止（タイマー解除）');
    else fail('ミラー停止が不完全');
  } catch (e) {
    fail(`例外: ${e instanceof Error ? e.stack : String(e)}`);
  }
  if (failures.length === 0) log('[SELFTEST] 全件通過');
  else log(`[SELFTEST] ${failures.length} 件失敗: ${failures.join(' / ')}`);
  finish(failures.length === 0 ? 0 : 2);
}

function maybeFinish(): void {  if (!stopping || finished) return;
  if (inFlight !== null) return;
  if (socket === null || socket.destroyed) return;
  // ADR: XR ループは Offer 待ちで空フレームを回し続けるため、読取側を開いたままの
  // 半閉鎖（socket.end）では切断を検知できない。finish と同じく破棄して切断として
  // 知らせ、exe に統計を出させる。inFlight が無いことは上で確認済みのため安全である。
  log('全借用の ACK が揃ったためソケットを閉じる');
  try { socket.destroy(); } catch { /* 無視 */ }
}

// ADR: ユーザー close・before-quit・SIGINT/SIGTERM・--seconds のいずれもこの
// 優雅な停止に入る。新規 paint を止めて未処理の Done を最大 3 秒待ち、揃えば
// maybeFinish がソケット破棄→close→exe 終了→finish の経路で終わる。
// 直接 finish(0) すると処理中の offer を必ず隔離して exit 2 になるため避ける。
// exe が無い動作・ソケットが無い場合は待っても意味が無いため即 finish する。
// 正常終了（全 Done 返却済み・隔離 0・exe 0）なら app も 0 にする。
// 猶予切れで未 ACK が残った場合のみ非ゼロ（2。隔離扱い）にする。
// drainedCode は猶予切れで残債が無い場合の終了コードであり、通常は 0 を渡す。
// --seconds の自動停止も正常終了として 0 を渡す（従来の 3 は使わない）。
function beginShutdown(drainedCode: number): void {
  if (finished) return;
  // --seconds とウィンドウ close の競合で二重開始すると猶予タイマーが
  // 作り直されて終了が延びるため、2 回目以降は何もしない。
  if (shutdownStarted) return;
  shutdownStarted = true;
  if (NO_EXE || SELF_TEST || socket === null || socket.destroyed) {
    finish(drainedCode);
    return;
  }
  log('終了手順を開始する（新規 paint を止め、未処理の Done を最大 3 秒待つ）');
  stopping = true;
  if (stopTimer !== null) {
    clearTimeout(stopTimer);
    stopTimer = null;
  }
  maybeFinish();
  if (graceTimer !== null) clearTimeout(graceTimer);
  // 未 ACK が残る猶予切れは隔離（失敗）扱いのため終了 2 にする。
  graceTimer = setTimeout(() => {
    log('grace 期限切れ。未 ACK を隔離して終了する');
    finish(inFlight !== null ? 2 : drainedCode);
  }, 3000);
}

function finish(code: number): void {
  if (finished) return;
  finished = true;
  // 終了確定。以後の新規 paint・pump を止める。
  stopping = true;
  for (const t of captureTimers) clearTimeout(t);
  captureTimers.length = 0;
  for (const t of controlCaptureTimers) clearTimeout(t);
  controlCaptureTimers.length = 0;
  // ミラー停止（タイマー解除）。送り済みフレームの後始末は要らない。
  stopMirror();
  stopVrMeter();
  if (statusTimer !== null) {
    clearInterval(statusTimer);
    statusTimer = null;
  }
  if (stats.mirrorFrames > 0) stats.mirrorCaptureAvgMs = mirrorTotalMs / stats.mirrorFrames;
  // XR ループは Offer 待ちで空フレームを回し続けるため、読取側を開いたままの
  // 半閉鎖では切断を検知できない。終了待ちの前に破棄して切断として知らせ、
  // exe に統計を出させる。未 ACK の借用は下で隔離し release しない。
  try {
    if (socket !== null && !socket.destroyed) socket.destroy();
  } catch { /* 無視 */ }
  socket = null;
  if (stopTimer) clearTimeout(stopTimer);
  if (graceTimer) clearTimeout(graceTimer);
  if (exeWaitTimer) clearTimeout(exeWaitTimer);
  if (vrNoRecordWarnTimer !== null) {
    clearTimeout(vrNoRecordWarnTimer);
    vrNoRecordWarnTimer = null;
  }
  if (vrFirstStampTimer !== null) {
    clearTimeout(vrFirstStampTimer);
    vrFirstStampTimer = null;
  }
  // offer 済み未 ACK は参照を held から外さず quarantined へ移して隔離する
  // （release しない。参照を捨てない）。未処理 paint は stale として即返すため
  // 未 offer の保留は残らない。
  if (inFlight !== null) {
    const tex = held.get(inFlight);
    held.delete(inFlight);
    if (tex !== undefined) quarantined.set(inFlight, tex);
    stats.quarantined++;
    log(`QUARANTINE borrow=${inFlight}（offer 済み未 ACK のため release しない。Electron 終了で回収）`);
    inFlight = null;
  }
  const intervals: number[] = [];
  for (let i = 1; i < paintTimes.length; i++) intervals.push(paintTimes[i] - paintTimes[i - 1]);
  const sorted = [...intervals].sort((a, b) => a - b);
  const q = (p: number): number => (sorted.length > 0 ? sorted[Math.min(sorted.length - 1, Math.floor(p * sorted.length))] : 0);
  const total = paintTimes.length > 1 ? paintTimes[paintTimes.length - 1] - paintTimes[0] : 0;
  stats.distinctHandles = handleCounts.size;
  stats.releaseAttempts = releaseAttempts;
  const summary = {
    exe: EXE_PATH, seconds: SECONDS, window: `${WIN_W}x${WIN_H}`,
    space: SPACE, renderScale: RENDER_SCALE, recordTtlMs: RECORD_TTL_MS,
    captureAt: CAPTURE_AT_LIST,
    gpuPid: currentGpuPid(),
    paints: paintTimes.length,
    fps: total > 0 ? intervals.length / (total / 1000) : 0,
    paintIntervalMs: { min: q(0), p50: q(0.5), p95: q(0.95), max: q(1) },
    exeSubmits: vrMeter.submits,
    exeUnknownId: vrMeter.unknownId,
    exePoseMismatch: vrMeter.poseMismatch,
    exeSpaceMismatch: vrMeter.spaceMismatch,
    exeExpired: vrMeter.expired,
    xrActive,
    xrDroppedRecords,
    mirrorVrFps: MIRROR_VR_FPS,
    ...stats,
  };
  try {
    fs.writeFileSync(statsPath, JSON.stringify(summary, null, 2));
  } catch (e) {
    log('stats write FAILED: ' + (e instanceof Error ? e.message : String(e)));
  }
  // 終了コードの決定。verify/copy 失敗・隔離・未返却・maxHeld 超過・offer 失敗が
  // 1 件でもあれば非ゼロ終了にする。
  let finalCode = code;
  const unreturned = stats.offers - stats.dones;
  const failures: string[] = [];
  if (stats.statusCopyFailed > 0) failures.push(`copyFail=${stats.statusCopyFailed}`);
  if (stats.statusVerifyFailed > 0) failures.push(`verifyFail=${stats.statusVerifyFailed}`);
  if (stats.quarantined > 0) failures.push(`quarantine=${stats.quarantined}`);
  if (unreturned !== 0) failures.push(`unreturned=${unreturned}`);
  if (stats.maxHeld > MAX_HELD) failures.push(`maxHeld=${stats.maxHeld}>${MAX_HELD}`);
  if (stats.offerFailures > 0) failures.push(`offerFailures=${stats.offerFailures}`);
  if (failures.length > 0) {
    if (finalCode === 0) finalCode = 2;
    log(`終了コードを非ゼロ化 (${failures.join(' ')})`);
    try {
      fs.writeFileSync(statsPath, JSON.stringify({ ...summary, failures, exitEscalated: true }, null, 2));
    } catch { /* 無視 */ }
  }
  flushLog();
  log(`M5 終了 code=${finalCode} offers=${stats.offers} dones=${stats.dones} drops=${stats.drops} ` +
    `stale=${stats.droppedStale} browseDrop=${stats.droppedBrowsing} ` +
    `ok=${stats.statusOk} copyFail=${stats.statusCopyFailed} verifyFail=${stats.statusVerifyFailed} ` +
    `records=${stats.renderRequests} xrDropped=${xrDroppedRecords} xrActive=${xrActive} ` +
    `inputs=${stats.inputStates} haptics=${stats.hapticSent} ` +
    `maxHeld=${stats.maxHeld} quarantine=${stats.quarantined} ` +
    `mirror=${stats.mirrorFrames} mirrorErr=${stats.mirrorErrors} ` +
    `capAvg=${stats.mirrorCaptureAvgMs.toFixed(1)}ms ` +
    `fwd=${stats.inputForwarded} fwdErr=${stats.inputErrors}`);
  flushLog();
  // コントロール窓を閉じる。破棄済み・終了中の作成失敗では何もしない。
  try {
    const ctl = controlRef;
    controlRef = null;
    if (ctl !== null && !ctl.isDestroyed()) ctl.close();
  } catch { /* 終了時は無視 */ }
  // exe 側の最終統計の出力を待つため、正常終了を最大 3 秒待ってから kill する。
  // 既に終了していれば即座に抜ける。切断検知の直後に kill すると exe の統計が欠ける。
  // ADR: 終了は stdio の EOF（close）で待つ。exit だけではパイプ中の最終統計を
  // 取りこぼす（ハーネスと同一の待ち方）。
  const waitExe = new Promise<void>((resolve) => {
    if (!exeProc || exeProc.exitCode !== null || exeProc.signalCode !== null) {
      resolve();
      return;
    }
    const t = setTimeout(resolve, 3000);
    exeProc.once('close', () => {
      clearTimeout(t);
      resolve();
    });
  });
  void waitExe.then(() => {
    try {
      if (exeProc && !exeProc.killed && exeProc.exitCode === null) exeProc.kill();
    } catch { /* 無視 */ }
    // exe の stdout を close（EOF）まで読み切った後に末尾断片を回収し、
    // XR転送統計が app ログに入ったかを確認する（終了証拠として必須）。
    try {
      flushExeStats();
    } catch { /* 計測の失敗は本線にしない */ }
    if (exeProc !== null && !exeStatsLogged) {
      log('警告: exe の XR転送統計を回収できない（exe が統計前に終了したか、出力の取りこぼし）');
    }
    // exe ログの書き切りを待ってから抜ける（統計行を落とさない）。詰まっても
    // 500ms で打ち切り、ハングしない。二重呼び出しはしない。
    let exited = false;
    const done = (): void => {
      if (exited) return;
      exited = true;
      flushLog();
      app.exit(finalCode);
    };
    const stream = exeLogStream;
    exeLogStream = null;
    if (stream === null) {
      done();
      return;
    }
    try {
      const t = setTimeout(done, 500);
      stream.once('finish', () => {
        clearTimeout(t);
        done();
      });
      stream.end();
    } catch {
      done();
    }
  });
}

async function createWindow(): Promise<BrowserWindow> {
  const win = new BrowserWindow({
    width: WIN_W,
    height: WIN_H,
    show: false,
    frame: false,
    // 画面作業領域への収め直しを防ぐ。生成直後に setContentSize で強制する。
    enableLargerThanScreen: true,
    webPreferences: {
      offscreen: { useSharedTexture: true },
      contextIsolation: true,
      nodeIntegration: false,
      // preload で自前のシムを読むため（ページ側には Node を出さない）
      sandbox: false,
      preload: path.join(__dirname, 'preload.js'),
      backgroundThrottling: false,
    },
  });
  // 生成直後の境界は画面作業領域に収め直されるため、setContentSize で強制する。
  win.setContentSize(WIN_W, WIN_H);
  win.webContents.setFrameRate(90);
  log(`window created: ${WIN_W}x${WIN_H} offscreen.useSharedTexture=true frameRate=90 target=${SECONDS}s`);
  // レコード転送先。RenderRequest の到着より先に設定する。
  winRef = win;

  // ADR: Electron の版で console-message の引数形状が違うため、unknown 境界で受ける。
  win.webContents.on('console-message', (...args: unknown[]) => {
    let level = 0;
    let message = '';
    const first = args[0];
    if (first !== null && typeof first === 'object' && 'message' in first) {
      const d = first as Record<string, unknown>;
      if (typeof d.message === 'string') message = d.message;
      if (typeof d.level === 'string') level = d.level === 'error' ? 2 : 0;
      else if (typeof d.level === 'number') level = d.level;
    } else {
      if (typeof args[1] === 'number') level = args[1];
      if (typeof args[2] === 'string') message = args[2];
    }
    // 本番ページの XR 到達確認（isSessionSupported/requestSession の有無）を
    // ログで追えるよう、XR 関連の行も拾う（通常ページの雑音は入れない）。
    const interesting = message.includes('[XRift M5]') || message.includes('[M5') || level === 2 ||
      /xr|immersive|session|haptic|pulse/i.test(message);
    if (interesting) {
      log('renderer-console: ' + message.replace(/%c|color:#a07cff/g, '').trim().slice(0, 2000));
    }
  });
  win.webContents.on('render-process-gone', (_event, details) => {
    log(`render-process-gone: reason=${details?.reason} exitCode=${details?.exitCode}`);
    stopping = true;
    maybeFinish();
    if (!graceTimer) {
      if (stopTimer) {
        clearTimeout(stopTimer);
        stopTimer = null;
      }
      graceTimer = setTimeout(() => {
        log('grace 期限切れ（renderer 異常）。未 ACK を隔離して終了する');
        finish(2);
      }, 8000);
    }
  });
  // ADR: paint の details 形状は版で違うため unknown 境界で受け、型ガードで絞る。
  // なお Electron の paint は第 1 引数に texture 担体が来る（ハーネスと同一の受け方）。
  win.webContents.on('paint', (...args: unknown[]) => {
    try {
      onPaint(args[0]);
    } catch (e) {
      log('paint handler error: ' + (e instanceof Error ? e.stack : String(e)));
      // 未登録の借用が残っていれば共通処理で返却する（握り潰さない）。
      releaseOrphanFromPaint(args[0], 'paint-outer-catch');
    }
  });

  // 認証・ポップアップ: window.open は VR 用 OSR には載せず、
  // 通常の可視ウィンドウで開く（OSR は VR 用の 1 枚だけ）。
  // 認証後の外部ホストへの遷移は許可し、止まった位置は will-navigate に残す。
  win.webContents.setWindowOpenHandler(({ url }) => {
    log(`window.open 要求 url=${url}（認証用ポップアップとして可視ウィンドウで開く）`);
    try {
      const popup = new BrowserWindow({
        width: 900,
        height: 700,
        show: true,
        autoHideMenuBar: true,
        webPreferences: {
          contextIsolation: true,
          nodeIntegration: false,
          sandbox: true,
          backgroundThrottling: false,
        },
      });
      void popup.loadURL(url).catch((e: unknown) => {
        log(`popup load 失敗 url=${url}: ${e instanceof Error ? e.message : String(e)}`);
      });
      popup.webContents.on('will-navigate', (_e, navUrl) => {
        log(`popup will-navigate url=${navUrl}`);
      });
    } catch (e) {
      log(`popup 作成に失敗 url=${url}: ${e instanceof Error ? e.message : String(e)}`);
    }
    return { action: 'deny' };
  });
  win.webContents.on('will-navigate', (_event, navUrl) => {
    // 外部ホストへの遷移は許可する（阻止しない）。VR ウィンドウがどこに留まったかは
    // この行で追える（app.xrift.net に留まることが期待値）。
    log(`will-navigate url=${navUrl}`);
  });
  win.webContents.on('did-finish-load', () => {
    log(`did-finish-load url=${win.webContents.getURL()}`);
    // ADR: 遷移で renderer が作り直されると旧セッションは失われるため、
    // xrActive を落としてブラウジングに戻す（VR サイズに固まる漏れの防止）。
    // 初回読み込み時は既に false のため何も起きない。
    setXrActive(false);
  });
  win.webContents.on('did-fail-load', (_event, code, desc, validatedUrl) => {
    log(`did-fail-load code=${code} desc=${desc} url=${validatedUrl}`);
  });

  // self-test では検証ページ（ボタン・入力欄つき）を強制する（引数の指定より優先）。
  const target = SELF_TEST
    ? { kind: 'file', file: path.join(__dirname, '..', 'pages', 'mirror-test.html') } as const
    : resolveTarget();
  if (target.kind === 'url') {
    await win.loadURL(target.url);
    log(`loaded url=${target.url}`);
  } else {
    await win.loadFile(target.file);
    log(`loaded page=${target.file}`);
  }
  scheduleCapture(win);

  if (SECONDS > 0) {
    stopTimer = setTimeout(() => {
      log(`${SECONDS}s 経過。新規 paint の受付を止める`);
      // inFlight が残れば猶予後に隔離して終了する（timeout を返却許可にしない）。
      // 残債が無ければ正常終了の 0 で終える（exe 有りでも変えない）。
      // 正常終了（全 Done 返却済み・隔離 0・exe 0）なら app も 0 になる。
      // ADR: 猶予は beginShutdown の 3 秒に抑える。通常の Done は 1 拍以内に来る
      // ため十分であり、--seconds 自動終了を 3〜5 秒に収める。
      beginShutdown(0);
    }, SECONDS * 1000);
  }
  return win;
}

function start(): void {
  // self-test は HMD・exe 無しで回すため、exe 不要扱いにする。
  const noExeMode = NO_EXE || SELF_TEST;
  // render-scale の範囲確認（0.2〜1.0。既定 0.5）。範囲外は exe が拒否するため先に落とす。
  const scaleNum = Number(RENDER_SCALE);
  if (!Number.isFinite(scaleNum) || scaleNum < 0.2 || scaleNum > 1.0) {
    log(`unknown --render-scale=${RENDER_SCALE}（0.2〜1.0 の範囲であること）`);
    finish(1);
    return;
  }
  if (addon === null && !noExeMode) {
    log('addon が無いため起動しない');
    finish(1);
    return;
  }
  // ADR: 下の exe 起動・パイプ部は既存の塊のまま条件で囲う（差分を小さく保つため再字下げしない）。
  if (!noExeMode) {
  const exeArgs = [
    '--xr-transfer',
    `--pipe=${PIPE_PATH}`,
    `--render-scale=${RENDER_SCALE}`,
    `--record-ttl-ms=${RECORD_TTL_MS}`,
    `--space=${SPACE}`,
    `--seconds=${SECONDS > 0 ? SECONDS + 30 : 3600}`,
    // ADR: 静止ページで paint が止まっても Offer 途絶で終了しないよう無効化する。
    // 終了はパイプ切断（finish 時の socket.destroy）で知らせる。
    '--producer-idle-timeout-ms=0',
    // フロー制御: 実パイプラインに合わせてレコード発行を抑える（exe 側は既定 off）。
    // 未処理画像がある間は発行せず、Offer 処理後に次を発行する。ハーネスには渡さない。
    // --no-pace-records（検証専用）のときは渡さず従来どおり毎フレーム発行させる。
    ...(NO_PACE ? [] : ['--pace-records' as const]),
  ];
  log(`exe: ${EXE_PATH} ${exeArgs.join(' ')}`);
  // exe の出力をファイルへ保存する（受理/不明 ID の内訳用）。端末にもそのまま出す。
  let exeStdio: 'inherit' | 'pipe' = 'inherit';
  exeLogStream = null;
  if (EXE_LOG !== '') {
    const exeLogPath = path.isAbsolute(EXE_LOG) ? EXE_LOG : path.join(logDir, EXE_LOG);
    try {
      fs.mkdirSync(path.dirname(exeLogPath), { recursive: true });
      exeLogStream = fs.createWriteStream(exeLogPath, { flags: 'w' });
      exeStdio = 'pipe';
      log(`exe の出力を保存: ${exeLogPath}`);
    } catch (e) {
      log(`exe ログを開けないため inherit に退避 (${EXE_LOG}): ${e instanceof Error ? e.message : String(e)}`);
    }
  }
  exeProc = spawn(EXE_PATH, exeArgs, { stdio: ['ignore', exeStdio, exeStdio] });
  if (exeStdio === 'pipe' && exeLogStream !== null) {
    const stream: fs.WriteStream = exeLogStream;
    const forward = (chunk: Buffer): void => {
      stream.write(chunk);
      // VR 計測用に提出・棄却の行を数える（描画経路には触らない）。
      // 終了統計（XR転送統計）は app ログにも残す（終了証拠として必須）。
      try {
        const text = chunk.toString('utf8');
        countExeMeterChunk(text);
        noteExeStatsChunk(text);
      } catch { /* 計測の失敗は本線にしない */ }
      try {
        process.stdout.write(chunk);
      } catch {
        // 親パイプ切断時（EPIPE）はファイル側の記録を優先し、落とさない
      }
    };
    exeProc.stdout?.on('data', forward);
    exeProc.stderr?.on('data', forward);
    // ADR: ストリームの end は finish が引き取る（close 待ち後の書き切り用）。
    // ここで end すると finish 側の flush 待ちが空振りして遅延するため触らない。
  }
  log(`exe spawned: pid=${exeProc.pid} pipe=${PIPE_PATH}`);
  exeProc.on('error', (e) => {
    log(`exe spawn error: ${e.message}`);
    finish(1);
  });
  exeProc.on('exit', (code, signal) => {
    exeCode = typeof code === 'number' ? code : 2;
    log(`exe exit: code=${code} signal=${signal}`);
    if (exeWaitTimer) {
      clearTimeout(exeWaitTimer);
      exeWaitTimer = null;
    }
    // パイプが既に無い（未接続死・close 済み）場合はコード確定。接続中は close 側の
    // 終了待ちに確定を委ねる（close→finish 確定の競合をなくす）。
    if (socket === null || pipeClosed) {
      finish(exeBaseCode());
    }
  });

  const server = net.createServer((s) => {
    if (socket !== null) {
      log('2 本目の接続を拒否');
      s.destroy();
      return;
    }
    socket = s;
    log('exe がパイプに接続');
    let acc = Buffer.alloc(0);
    s.on('data', (chunk: Buffer) => {
      acc = Buffer.concat([acc, chunk]);
      while (acc.length >= MSG_LEN) {
        const m = acc.subarray(0, MSG_LEN);
        acc = acc.subarray(MSG_LEN);
        if (m.readUInt32LE(0) !== MAGIC || m.readUInt16LE(4) !== VERSION) {
          log('不正なメッセージを破棄（magic/version の不一致）');
          continue;
        }
        const msgType = m.readUInt16LE(6);
        // exe→main の描画要求。借用台帳には触れず renderer へ渡す。
        if (msgType === TYPE_RENDER_REQUEST) {
          forwardRecord(m);
          continue;
        }
        // exe→main の入力スナップショット。台帳には触れず記録する。
        if (msgType === TYPE_INPUT_STATE) {
          onInputState(m);
          continue;
        }
        if (msgType !== TYPE_DONE) {
          log(`未知のメッセージ種別を破棄 type=${msgType}`);
          continue;
        }
        const borrowId = Number(m.readBigUInt64LE(8));
        const status = m.readUInt32LE(36);
        onDone(borrowId, status);
      }
    });
    s.on('close', () => {
      log('パイプ切断を検知');
      socket = null;
      pipeClosed = true;
      if (finished) return;
      if (exeCode !== null) {
        // exe は既に終了している。exe のコードで確定する。
        finish(exeBaseCode());
      } else {
        // exe の終了を待ってから確定する（タイムアウト付き）。
        log('exe の終了待ちを開始（上限 5 秒）');
        exeWaitTimer = setTimeout(() => {
          exeWaitTimer = null;
          log('exe 終了待ちがタイムアウト。非ゼロ扱いで終了する');
          finish(2);
        }, EXE_WAIT_MS);
      }
    });
    s.on('error', (e) => {
      log(`パイプエラー: ${e.message}`);
    });
  });
  server.on('error', (e) => {
    log(`パイプサーバ失敗: ${(e as Error).message}`);
    finish(1);
  });
  server.listen(PIPE_PATH, () => {
    log(`pipe server listening: ${PIPE_PATH}`);
  });
  } else {
    log('exe 無し動作のため spawn・パイプサーバを飛ばす（ミラー・入力転送のみ）');
  }

  // renderer からの haptics 要求を exe へ中継する。境界で数値を検証する。
  ipcMain.on('xr:haptic', (_event, ...args: unknown[]) => {
    const [hand, intensity, durationMs] = args;
    if (typeof hand !== 'number' || typeof intensity !== 'number' || typeof durationMs !== 'number') {
      log('haptics 要求を破棄（引数が数値ではない）');
      return;
    }
    if (!Number.isFinite(hand) || !Number.isFinite(intensity) || !Number.isFinite(durationMs)) return;
    sendHaptic(Math.trunc(hand), intensity, Math.trunc(durationMs));
  });
  // コントロール窓からの入力転送を OSR へ渡す。境界検証は mirror-input が担う。
  ipcMain.on('mirror:input', (_event, msg: unknown) => {
    handleMirrorInput(msg);
  });
  // renderer のセッション開始・終了の通知。真で xrActive を立て、偽で落とす。
  // 境界は真偽値だけを受け、他の型は破棄する。
  ipcMain.on('xr:session', (_event, msg: unknown) => {
    if (typeof msg !== 'boolean') {
      log('xr:session 通知を破棄（真偽値ではない）');
      return;
    }
    setXrActive(msg);
  });
  // シムの初回制御ブロック描画の通知。Offer 開始の合図にする。
  // 境界は文字列だけを受け、他の型は破棄する。
  ipcMain.on('xr:stamped', (_event, msg: unknown) => {
    if (typeof msg !== 'string' || msg === '') return;
    if (!xrActive || vrPixelsValid) return;
    vrPixelsValid = true;
    if (vrFirstStampTimer !== null) {
      clearTimeout(vrFirstStampTimer);
      vrFirstStampTimer = null;
    }
    log(`[M5 VR開始] 初回描画を確認（renderId=${msg}。Offer を開始する）`);
  });
  // ページの到達確認用。exe 生存とパイプ接続だけを見る（HMD 装着の有無は見ない）。
  // シムは準備前に最大 15 秒ポーリングするため、最初の数回と結果変化だけ記録する。
  let probeCount = 0;
  let lastProbeOk: boolean | null = null;
  ipcMain.handle('xr:probe', () => {
    const res = {
      ok: exeProc !== null && exeProc.exitCode === null && socket !== null && !socket.destroyed,
      exeAlive: exeProc !== null && exeProc.exitCode === null,
      pipeConnected: socket !== null && !socket.destroyed,
    };
    probeCount++;
    if (probeCount <= 5 || res.ok !== lastProbeOk) {
      log(`xr:probe #${probeCount} ok=${res.ok} exeAlive=${res.exeAlive} pipe=${res.pipeConnected}`);
    }
    lastProbeOk = res.ok;
    return res;
  });

  createWindow().then((osr) => {
    // OSR の後にコントロール窓を作り、ミラー・状態配信を開始する。
    // self-test はここから自動検証に入り、結果で終了する。
    createControlWindow().then((control) => {
      scheduleControlCapture(control);
      startMirror();
      startStatusBroadcast();
      startVrMeter();
      if (SELF_TEST) {
        // HARD TIMEOUT の前に終わらせるための保険（self-test 専用）。
        setTimeout(() => {
          log('[SELFTEST] 制限時間切れのため中断する');
          finish(2);
        }, 60000);
        void runSelfTest(osr, control);
      }
    }).catch((e: unknown) => {
      log('createControlWindow FAILED: ' + (e instanceof Error ? e.stack : String(e)));
      if (SELF_TEST) finish(2);
    });
  }).catch((e: unknown) => {
    log('createWindow FAILED: ' + (e instanceof Error ? e.stack : String(e)));
    finish(1);
  });
}

app.whenReady().then(() => {
  log(`electron=${process.versions.electron} chrome=${process.versions.chrome} node=${process.versions.node}`);
  if (SECONDS > 0) {
    // 終了保険（計測 + 猶予 + 余裕）。保険発火も finish 経由で台帳を守る。
    setTimeout(() => {
      log('HARD TIMEOUT 発火');
      finish(2);
    }, (SECONDS + 60) * 1000);
  }
  start();
});
process.on('SIGINT', () => {
  log('SIGINT を受けた。終了手順に入る');
  beginShutdown(0);
});
process.on('SIGTERM', () => {
  log('SIGTERM を受けた。終了手順に入る');
  beginShutdown(0);
});
// ADR: 終了はすべて beginShutdown→finish 経由に一本化する。コントロール窓の
// close が先行した場合は finished 旗で何もしない。before-quit では既定の終了を
// 止めて finish の非同期待ち（exe の統計回収）を優先し、finish 側の app.exit で抜ける。
// app.exit() 自体は before-quit を再発火しないため二重終了にならない。
app.on('before-quit', (event) => {
  if (!finished) {
    event.preventDefault();
    log('before-quit を受けた。終了手順に入る');
    beginShutdown(0);
  }
});
app.on('window-all-closed', () => {
  if (!finished) {
    log('window-all-closed を受けた。終了手順に入る');
    beginShutdown(0);
  }
});
