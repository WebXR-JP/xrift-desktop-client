/**
 * M5 新アプリの main プロセス。検証用ハーネス（`native_new/harness/electron/main.js`）の
 * 借用台帳・パイプサーバ・終了経路を TypeScript に整理して流用したものである。
 *
 * 役割: パイプサーバ + 借用台帳 + `release()` 専任。提出には関与しない。
 *  - OSR ウィンドウ（show:false、offscreen useSharedTexture）で paint を受ける
 *  - 借用は「処理中 1 枚 + 保留 1 枚」まで。超過した古い保留は即 release する
 *  - Done を受けたらその借用だけ release する（二重 release しない）
 *  - exe の終了・切断時は、未 offer の保留だけ release する。offer 済み未 ACK は
 *    GPU 完了の証拠が無いため release せず隔離する。Electron 終了で回収する
 *  - `RenderRequest` は renderer へそのまま渡す（pose/FOV を推測・付与しない）。
 *    `InputState` も同様に渡す。`HapticCommand` は renderer からの IPC を中継する
 *
 * @packageDocumentation
 */
import { app, BrowserWindow, ipcMain } from 'electron';
import { spawn, type ChildProcess } from 'node:child_process';
import * as net from 'node:net';
import * as path from 'node:path';
import * as fs from 'node:fs';
import {
  MSG_LEN, MAGIC, VERSION,
  TYPE_DONE, TYPE_RENDER_REQUEST, TYPE_INPUT_STATE,
  STATUS_OK, FORMAT_BGRA,
  decodeRecord, decodeInput, makeOffer, makeHaptic,
  type InputSnapshot,
} from './protocol';

// VR 中はデスクトップのウィンドウが隠れていても描画・タイマーを止めない。
// appendSwitch は ready より前に呼ぶ（ready 後に足しても効かない）。
app.commandLine.appendSwitch('disable-background-timer-throttling');
app.commandLine.appendSwitch('disable-renderer-backgrounding');
app.commandLine.appendSwitch('disable-backgrounding-occluded-windows');
app.commandLine.appendSwitch('disable-features', 'CalculateNativeWinOcclusion');
app.commandLine.appendSwitch('autoplay-policy', 'no-user-gesture-required');

function arg(name: string, def: string): string {
  const prefix = `--${name}=`;
  for (const a of process.argv.slice(2)) {
    if (a.startsWith(prefix)) return a.slice(prefix.length);
  }
  return def;
}

const EXE_PATH = arg('exe', path.join(__dirname, '..', '..', 'native_new', 'build', 'windows', 'Release', 'xrift_new.exe'));
const PIPE_PATH = arg('pipe', `\\\\.\\pipe\\xrift_m5_${process.pid}`);
const RENDER_SCALE = arg('render-scale', '0.5');
const SECONDS = Math.max(0, parseInt(arg('seconds', '0'), 10) || 0);
const WIN_W = parseInt(arg('width', '2048'), 10) || 2048;
const WIN_H = parseInt(arg('height', '1024'), 10) || 1024;
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
const ADDON_PATH = arg(
  'addon',
  path.join(__dirname, '..', '..', 'native_new', 'transfer-addon', 'build', 'Release', 'xrift_transfer_addon.node'),
);
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
  log('addon loaded: xrift_transfer_addon.node');
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
let pending: number | null = null; // 保留（最大 1）。offer 前なら即 release 可
const MAX_HELD = 2; // 同時保持の上限（処理中 1 + 保留 1）
let stopping = false; // 計測終了後。新規 paint は即 release する
let finished = false;
const stats = {
  offers: 0, dones: 0, drops: 0, offerFailures: 0, staleDones: 0,
  quarantined: 0, releasedPending: 0, maxHeld: 0,
  statusOk: 0, statusCopyFailed: 0, statusVerifyFailed: 0,
  noTexture: 0, formatMismatch: 0, missingHandle: 0,
  distinctHandles: 0, firstCodedSize: null as { width: number; height: number } | null,
  renderRequests: 0, // exe から受けた RenderRequest の件数。
  inputStates: 0, // exe から受けた InputState の件数。
  lastInput: null as InputSnapshot | null, // 最後に受けた InputState の要約（統計用）。
  hapticSent: 0, // exe へ送った HapticCommand の件数。
  captures: 0, // 検証専用の --capture-at で保存した PNG の枚数（描画経路に影響しない）。
  releaseAttempts: 0, // release() の試行回数。
};
const handleCounts = new Map<string, number>();
const paintTimes: number[] = [];
let exeProc: ChildProcess | null = null;
let socket: net.Socket | null = null;
let winRef: BrowserWindow | null = null; // レコード転送先（createWindow が設定する）。
let stopTimer: NodeJS.Timeout | null = null;
let graceTimer: NodeJS.Timeout | null = null;
const captureTimers: NodeJS.Timeout[] = [];
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

// inFlight が空いていれば保留を繰り上げる。複製失敗時はその借用を捨てて次を試す。
// 停止中は登録・pump を止めるため何もしない。
function pumpQueue(meta: Map<number, { width: number; height: number; ntHandle: Buffer }>): void {
  if (stopping) return;
  while (inFlight === null && pending !== null && socket !== null && !socket.destroyed) {
    const b = pending;
    pending = null;
    const m = meta.get(b);
    meta.delete(b);
    if (m === undefined) continue;
    if (makeOfferToExe(b, m.width, m.height, m.ntHandle)) {
      inFlight = b;
    }
  }
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

  // 新規借用を登録する前に古い保留の release を済ませ、maxHeld が上限を超えない
  // ようにする（登録後に追い出すと瞬間的に 3 枚になり maxHeld が汚れる）。
  const meta = onPaintMeta;
  if (inFlight !== null && pending !== null) {
    const old = pending;
    pending = null;
    meta.delete(old);
    stats.drops++;
    releaseOnce(old, 'superseded');
  }

  // 借用登録（release の所有権を台帳が持つ）。
  const borrowId = nextBorrowId++;
  held.set(borrowId, texture);
  noteHeld();
  meta.set(borrowId, { width: coded.width, height: coded.height, ntHandle: Buffer.from(nt as Buffer) });

  if (socket === null || socket.destroyed) {
    // exe 未接続。保留に積まず即返す（転送前 drop）。
    meta.delete(borrowId);
    stats.drops++;
    releaseOnce(borrowId, 'not-connected');
    return;
  }
  if (inFlight === null) {
    const m = meta.get(borrowId);
    meta.delete(borrowId);
    if (m !== undefined && makeOfferToExe(borrowId, m.width, m.height, m.ntHandle)) {
      inFlight = borrowId;
    }
    return;
  }
  if (pending === null) {
    pending = borrowId;
    return;
  }
  // ここには来ないはず（登録前に古い保留を追い出している）。来たら異常として記録する。
  log(`異常: 保留が二重になった borrow=${borrowId} old=${pending}`);
  meta.delete(pending);
  stats.drops++;
  releaseOnce(pending, 'superseded-unexpected');
  pending = borrowId;
}
const onPaintMeta = new Map<number, { width: number; height: number; ntHandle: Buffer }>();
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
    // 停止中は pump しない。
    if (!stopping) pumpQueue(onPaintMeta);
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
let lastReqSize: { w: number; h: number } | null = null;
let recordLogCount = 0;
function forwardRecord(m: Buffer): void {
  const rec = decodeRecord(m);
  if (rec === null) {
    log('RenderRequest の復号に失敗（配置不正）');
    return;
  }
  stats.renderRequests++;
  if (rec.reqWidth > 0 && rec.reqHeight > 0 &&
      winRef !== null && !winRef.isDestroyed()) {
    const targetW = rec.reqWidth * 2;
    const targetH = rec.reqHeight;
    const [curW, curH] = winRef.getContentSize();
    if (lastReqSize === null || lastReqSize.w !== targetW || lastReqSize.h !== targetH ||
        curW !== targetW || curH !== targetH) {
      try {
        winRef.setContentSize(targetW, targetH);
        log(`window リサイズ: ${curW}x${curH} → ${targetW}x${targetH}（req ${rec.reqWidth}x${rec.reqHeight}/眼 renderId=${rec.renderId}）`);
      } catch (e) {
        log(`window リサイズ失敗 ${targetW}x${targetH}: ${e instanceof Error ? e.message : String(e)}`);
      }
      lastReqSize = { w: targetW, h: targetH };
    }
    // 最初の数回だけ、要求と窓の対応を記録する（以後の canvas 側は renderer が出す）。
    if (recordLogCount < 3) {
      recordLogCount++;
      const [nowW, nowH] = winRef.getContentSize();
      log(`[M5記録] #${recordLogCount} renderId=${rec.renderId} req=${rec.reqWidth}x${rec.reqHeight}/眼 window=${nowW}x${nowH}`);
    }
    winRef.webContents.send('xr:record', rec);
  } else if (winRef !== null && !winRef.isDestroyed()) {
    winRef.webContents.send('xr:record', rec);
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

function maybeFinish(): void {
  if (!stopping || finished) return;
  if (inFlight !== null) return;
  if (socket === null || socket.destroyed) return;
  log('全借用の ACK が揃ったためソケットを閉じる（未 offer の保留は終了時に返却）');
  try { socket.end(); } catch { /* 無視 */ }
}

function finish(code: number): void {
  if (finished) return;
  finished = true;
  // 終了確定。以後の新規 paint・pump を止める。
  stopping = true;
  for (const t of captureTimers) clearTimeout(t);
  captureTimers.length = 0;
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
  // 未 offer の保留だけ release する。offer 済み未 ACK は参照を held から外さず
  // quarantined へ移して隔離する（release しない。参照を捨てない）。
  if (pending !== null) {
    onPaintMeta.delete(pending);
    if (releaseOnce(pending, 'shutdown-pending')) stats.releasedPending++;
    pending = null;
  }
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
    space: SPACE, renderScale: RENDER_SCALE, captureAt: CAPTURE_AT_LIST,
    gpuPid: currentGpuPid(),
    paints: paintTimes.length,
    fps: total > 0 ? intervals.length / (total / 1000) : 0,
    paintIntervalMs: { min: q(0), p50: q(0.5), p95: q(0.95), max: q(1) },
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
    `ok=${stats.statusOk} copyFail=${stats.statusCopyFailed} verifyFail=${stats.statusVerifyFailed} ` +
    `records=${stats.renderRequests} inputs=${stats.inputStates} haptics=${stats.hapticSent} ` +
    `maxHeld=${stats.maxHeld} quarantine=${stats.quarantined}`);
  flushLog();
  // exe 側の最終統計の出力を待つため、正常終了を最大 3 秒待ってから kill する。
  // 既に終了していれば即座に抜ける。切断検知の直後に kill すると exe の統計が欠ける。
  const waitExe = new Promise<void>((resolve) => {
    if (!exeProc || exeProc.exitCode !== null || exeProc.signalCode !== null) {
      resolve();
      return;
    }
    const t = setTimeout(resolve, 3000);
    exeProc.once('exit', () => {
      clearTimeout(t);
      resolve();
    });
  });
  void waitExe.then(() => {
    try {
      if (exeProc && !exeProc.killed && exeProc.exitCode === null) exeProc.kill();
    } catch { /* 無視 */ }
    flushLog();
    app.exit(finalCode);
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
  });
  win.webContents.on('did-fail-load', (_event, code, desc, validatedUrl) => {
    log(`did-fail-load code=${code} desc=${desc} url=${validatedUrl}`);
  });

  const target = resolveTarget();
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
      stopping = true;
      maybeFinish();
      // inFlight が残れば猶予後に隔離して終了する（timeout を返却許可にしない）。
      graceTimer = setTimeout(() => {
        log('grace 期限切れ。未 ACK を隔離して終了する');
        // 未 ACK が残る猶予切れは隔離（失敗）扱いのため終了 2 にする。残債が無ければ 3。
        finish(inFlight !== null ? 2 : 3);
      }, 8000);
    }, SECONDS * 1000);
  }
  return win;
}

function start(): void {
  if (addon === null) {
    log('addon が無いため起動しない');
    finish(1);
    return;
  }
  const exeArgs = [
    '--xr-transfer',
    `--pipe=${PIPE_PATH}`,
    `--render-scale=${RENDER_SCALE}`,
    `--space=${SPACE}`,
    `--seconds=${SECONDS > 0 ? SECONDS + 30 : 3600}`,
  ];
  log(`exe: ${EXE_PATH} ${exeArgs.join(' ')}`);
  // exe の出力をファイルへ保存する（受理/不明 ID の内訳用）。端末にもそのまま出す。
  let exeStdio: 'inherit' | 'pipe' = 'inherit';
  let exeLogStream: fs.WriteStream | null = null;
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
      try {
        process.stdout.write(chunk);
      } catch {
        // 親パイプ切断時（EPIPE）はファイル側の記録を優先し、落とさない
      }
    };
    exeProc.stdout?.on('data', forward);
    exeProc.stderr?.on('data', forward);
    exeProc.on('exit', () => {
      try { stream.end(); } catch { /* 終了時は無視 */ }
    });
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

  createWindow().catch((e: unknown) => {
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
  log('SIGINT を受けた。終了する');
  finish(0);
});
app.on('window-all-closed', () => { /* 何もしない（OSR 収集中は閉じない） */ });
