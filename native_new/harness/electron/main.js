// M1 検証ハーネスの main プロセス。検証用であり本番 TS ではない。
// 役割: パイプサーバ + 借用台帳 + release 専任。提出には関与しない。
//  - BrowserWindow（show:false、offscreen useSharedTexture）で paint を受ける
//  - 借用は「処理中 1 枚 + 保留 1 枚」まで。超過した古い保留は即 release する
//  - Done を受けたらその借用だけ release する（二重 release しない）
//  - exe の終了・切断時は、未 offer の保留だけ release する。offer 済み未 ACK は
//    GPU 完了の証拠が無いため release せず隔離し（DESIGN §6②）、Electron 終了で回収する
'use strict';
const { app, BrowserWindow } = require('electron');
const { spawn } = require('node:child_process');
const net = require('node:net');
const path = require('node:path');
const fs = require('node:fs');

function arg(name, def) {
  const p = `--${name}=`;
  for (const a of process.argv.slice(2)) {
    if (a.startsWith(p)) return a.slice(p.length);
  }
  return def;
}
const EXE_PATH = arg('exe', path.join(__dirname, '..', '..', 'build', 'windows', 'Release', 'xrift_new.exe'));
const SECONDS = Math.max(1, parseInt(arg('seconds', '60'), 10) || 60);
const WIN_W = parseInt(arg('width', '4096'), 10) || 4096;
const WIN_H = parseInt(arg('height', '2048'), 10) || 2048;
// 検証 5 用: exe へ --fail-after=N を渡し、人為的に非ゼロ終了させる（既定 0=無効）。
const FAIL_AFTER = Math.max(0, parseInt(arg('fail-after', '0'), 10) || 0);
// M1b 障害注入フック（既定は全て 0=off）。
// exe 側: --inject-unconfirmed=N（N 件目を完了未確認として扱う）、--delay-ms=N（consumer 遅延）。
// harness 側: --drop-done-at=N（N 個目の Done を捨て、ACK 消失を模擬する）。
const INJECT_UNCONFIRMED = Math.max(0, parseInt(arg('inject-unconfirmed', '0'), 10) || 0);
const DELAY_MS = Math.max(0, parseInt(arg('delay-ms', '0'), 10) || 0);
const DROP_DONE_AT = Math.max(0, parseInt(arg('drop-done-at', '0'), 10) || 0);
// M1b 検査妥当性用: レンダラへ注入フックを渡す（既定は全て 0=off）。
// N フレーム目だけ描画を壊し、exe 側の GPU 検査が不一致を数えることを確かめる。
const INJECT_BAD_TILE = Math.max(0, parseInt(arg('inject-bad-tile', '0'), 10) || 0);
const INJECT_STALE_TILE = Math.max(0, parseInt(arg('inject-stale-tile', '0'), 10) || 0);
const INJECT_BAD_CORNER = Math.max(0, parseInt(arg('inject-bad-corner', '0'), 10) || 0);
const INJECT_BAD_BIT = Math.max(0, parseInt(arg('inject-bad-bit', '0'), 10) || 0);
// exe 側の全画面 GPU 検査。--no-gpu-check で無効化できる（既定 on）。
const NO_GPU_CHECK = process.argv.includes('--no-gpu-check');
// 診断モード（既定 vulkan=Vulkan 経路。D3D11 デバイスを作らない）。exe へ --mode として渡す。
// d3d-open-only は D3D11 Open 経路の診断専用であり、長時間動かすとメモリが増える。
// 借用台帳・ACK・release はどのモードでも同じ経路を通す。
const MODE = arg('mode', 'vulkan');
// exe 側の 1 秒周期メモリ CSV（既定 ''=出さない）。--mem-log=<path> として渡す。
const EXE_MEM_LOG = arg('mem-log', '');
if (!['vulkan', 'd3d-open-only', 'no-gpu'].includes(MODE)) {
  console.error(`unknown --mode=${MODE}（vulkan / d3d-open-only / no-gpu のいずれか）`);
  process.exitCode = 1;
  app.whenReady().then(() => app.exit(1));
  return;
}

// M2 XR 提出モード（検証用フック）。--m2 で index-m2.html を描き、exe を
// --xr-transfer で起動する。exe→main の RenderRequest を受けてレンダラへ渡す。
// --inject-stale-record=N は N フレーム目の RenderRequest に旧 pose 混入の合図を
// 載せるよう exe へ渡し、合図付き要求の pose を 1 つ前のもので描かせる
// （不一致として棄却される想定。既定 0=無効）。
const M2 = process.argv.includes('--m2');
const INJECT_STALE_RECORD = Math.max(0, parseInt(arg('inject-stale-record', '0'), 10) || 0);
const RENDER_SCALE = arg('render-scale', '0.5');
const NO_SUBMIT_VERIFY = process.argv.includes('--no-submit-verify');

// プロトコル（native_new/src/transfer/protocol.h と同じ配置。192 バイト LE）。
// M2 で 64B→192B 化した。先頭 64B の配置は M1 と同じである。
const MSG_LEN = 192;
const MAGIC = 0x54465258;
const VERSION = 2;
const TYPE_OFFER = 1;
const TYPE_DONE = 2;
const TYPE_RENDER_REQUEST = 3;
const FORMAT_BGRA = 0;
const STATUS_OK = 0;

const outDir = __dirname;
const logPath = path.join(outDir, 'harness-run.log');
const statsPath = path.join(outDir, 'harness-stats.json');
const logLines = [];
function log(msg) {
  const line = `[${new Date().toISOString()}] ${msg}`;
  logLines.push(line);
  try {
    console.log(line);
  } catch {
    // 親パイプ切断時（EPIPE）はファイル側の記録を優先し、落とさない
  }
}
function flushLog() {
  try {
    fs.writeFileSync(logPath, logLines.join('\n') + '\n');
  } catch { /* 終了時は無視 */ }
}
process.on('uncaughtException', (e) => {
  if (e && e.code === 'EPIPE') return;
  log('FATAL uncaughtException: ' + (e && e.stack || e));
  flushLog();
  app.exit(1);
});

let addon = null;
try {
  addon = require(path.join(__dirname, '..', '..', 'transfer-addon', 'build', 'Release', 'xrift_transfer_addon.node'));
  log('addon loaded: xrift_transfer_addon.node');
} catch (e) {
  log('addon load FAILED: ' + (e && e.message));
  flushLog();
  process.exitCode = 1;
  app.whenReady().then(() => app.exit(1));
  return;
}

// 借用台帳。release() は main プロセスだけが呼び、1 借用 1 回に限る。
let nextBorrowId = 1;
const held = new Map(); // borrowId -> texture（release 前に保持）
const quarantined = new Map(); // borrowId -> texture（release 失敗・完了未確認。触らない）
const quarantinedPreBorrow = []; // 借用登録前に release 失敗した texture 参照の退避先
let releaseFailed = false; // release() 自体が失敗したら世代終了扱い（非ゼロ終了）
let inFlight = null;    // offer 済み・ACK 待ちの borrowId（最大 1）
let pending = null;     // 保留（最大 1）。offer 前なら即 release 可
const MAX_HELD = 2;     // 同時保持の上限（処理中 1 + 保留 1）
let stopping = false;   // 計測終了後。新規 paint は即 release する
let finished = false;
const stats = {
  offers: 0, dones: 0, drops: 0, offerFailures: 0, staleDones: 0, droppedDone: 0,
  quarantined: 0, releasedPending: 0, maxHeld: 0,
  statusOk: 0, statusCopyFailed: 0, statusVerifyFailed: 0,
  noTexture: 0, formatMismatch: 0, missingHandle: 0,
  distinctHandles: 0, firstCodedSize: null,
  renderRequests: 0, // M2: exe から受けた RenderRequest の件数。
};
const handleCounts = new Map();
// ソースハンドルの初出時刻（複製前の生値 hex -> 初出時刻 ms）。
// paint の textureInfo.handle.ntHandle の生値（main が見ている値。exe へ送る
// 複製後の値ではない）を集計し、再利用か毎回新規かの判定材料にする。
const handleFirstSeen = new Map();
const paintTimes = [];
let exeProc = null;
let socket = null;
let doneSeq = 0; // 受信した Done の通番（--drop-done-at 用。既定 0=無効）。
let winRef = null; // M2 のレコード転送先（createWindow が設定する）。
// GPU プロセスの pid（app.getAppMetrics() から取る。GPU メモリ計測との突き合わせ用）。
let gpuPidStart = null;
let stopTimer = null;
let graceTimer = null;
// 終了コード確定の状態。パイプ close で finish(0) を確定し、後の exe 非ゼロを
// 無視する競合をなくすため、close → exe 終了待ち（タイムアウト付き）→ 確定の順にする。
let exeCode = null; // exe の終了コード（signal 死は非ゼロ扱い）。未終了は null。
let pipeClosed = false;
let exeWaitTimer = null;
const EXE_WAIT_MS = 5000; // パイプ close 後の exe 終了待ち上限。
function exeBaseCode() { return exeCode === 0 ? 0 : 2; }

function releaseOnce(borrowId, why) {
  const tex = held.get(borrowId);
  if (tex === undefined) return false;
  try {
    tex.release();
  } catch (e) {
    // 台帳から先に消さない。参照を quarantined に残し、世代終了扱いにする。
    held.delete(borrowId);
    quarantined.set(borrowId, tex);
    stats.quarantined++;
    releaseFailed = true;
    stopping = true;
    log(`release FAILED borrow=${borrowId} why=${why}: ${e && e.message}（隔離。世代終了扱い）`);
    return false;
  }
  held.delete(borrowId);
  return true;
}

// 借用登録前（台帳に載る前）の texture を返す共通処理。release() 自体が失敗したら
// 参照を保持して隔離し、世代終了扱いにする（握り潰さない）。戻り値は返却の成否。
function releaseUnregistered(texture, why) {
  try {
    texture.release();
  } catch (e) {
    quarantinedPreBorrow.push(texture);
    stats.quarantined++;
    releaseFailed = true;
    stopping = true;
    log(`release(登録前) FAILED why=${why}: ${e && e.message}（隔離。世代終了扱い）`);
    return false;
  }
  return true;
}

// paint ハンドラの外側 catch 用。onPaint が登録・返却のどちらも済ませずに投げた
// 借用があれば、共通処理で返却する。台帳管理下（held/quarantined）のものは触らない。
function releaseOrphanFromPaint(details, where) {
  const tex = details && details.texture;
  if (!tex) return;
  for (const v of held.values()) if (v === tex) return;
  for (const v of quarantined.values()) if (v === tex) return;
  if (quarantinedPreBorrow.includes(tex)) return;
  log(`paint 外側 catch から未登録借用を返却 (${where})`);
  releaseUnregistered(tex, where);
}

function noteHeld() {
  if (held.size > stats.maxHeld) stats.maxHeld = held.size;
}

// GPU プロセスの pid を app.getAppMetrics() から取る。取れなければ null。
// 1 秒周期の GPU メモリ計測（Get-Counter 側）のインスタンス名（pid_XXXX 形式）と
// 突き合わせるための鍵である。
function currentGpuPid() {
  try {
    const metrics = app.getAppMetrics();
    const gpu = metrics.find((m) => String(m && m.type).toLowerCase() === 'gpu');
    if (gpu && typeof gpu.pid === 'number') return gpu.pid;
    const types = metrics.map((m) => `${m && m.type}:${m && m.pid}`).join(' ');
    log(`GPU プロセスが見つからない (types=${types})`);
    return null;
  } catch (e) {
    log(`getAppMetrics 失敗: ${e && e.message}`);
    return null;
  }
}

function makeOffer(borrowId, width, height, ntHandle) {
  // 複製して送る。失敗したらその借用だけ release して失敗扱いにする。
  let dup;
  try {
    dup = addon.duplicateHandle(Buffer.from(ntHandle), exeProc.pid);
  } catch (e) {
    stats.offerFailures++;
    log(`duplicateHandle THROW borrow=${borrowId}: ${e && e.message}`);
    releaseOnce(borrowId, 'dup-throw');
    return false;
  }
  if (!dup || dup.ok !== true || typeof dup.handle !== 'bigint') {
    stats.offerFailures++;
    log(`duplicateHandle FAILED borrow=${borrowId}: ${JSON.stringify(dup)}`);
    releaseOnce(borrowId, 'dup-failed');
    return false;
  }
  const buf = Buffer.alloc(MSG_LEN);
  buf.writeUInt32LE(MAGIC, 0);
  buf.writeUInt16LE(VERSION, 4);
  buf.writeUInt16LE(TYPE_OFFER, 6);
  buf.writeBigUInt64LE(BigInt(borrowId), 8);
  buf.writeUInt32LE(width, 16);
  buf.writeUInt32LE(height, 20);
  buf.writeUInt32LE(FORMAT_BGRA, 24);
  buf.writeBigUInt64LE(dup.handle, 28);
  buf.writeUInt32LE(0, 36);
  try {
    socket.write(buf);
  } catch (e) {
    stats.offerFailures++;
    log(`offer send FAILED borrow=${borrowId}: ${e && e.message}`);
    releaseOnce(borrowId, 'send-failed');
    return false;
  }
  stats.offers++;
  return true;
}

// inFlight が空いていれば保留を繰り上げる。複製失敗時はその借用を捨てて次を試す。
// 停止中は登録・pump・submit をすべて止めるため何もしない。
function pumpQueue(meta) {
  if (stopping) return;
  while (inFlight === null && pending !== null && socket !== null && !socket.destroyed) {
    const b = pending;
    pending = null;
    const m = meta.get(b);
    meta.delete(b);
    if (m === undefined) continue;
    if (makeOffer(b, m.width, m.height, m.ntHandle)) {
      inFlight = b;
    }
  }
}

function onPaint(details) {
  paintTimes.push(Date.now());
  // GPU プロセスは起動直後にいないことがあるため、初 paint で再取得する。
  if (gpuPidStart === null) {
    gpuPidStart = currentGpuPid();
    if (gpuPidStart !== null) log(`GPU プロセス pid=${gpuPidStart}（初 paint 時に取得）`);
  }
  const texture = details && details.texture;
  if (!texture) {
    stats.noTexture++;
    return;
  }
  let ti = null;
  try {
    ti = texture.textureInfo;
  } catch (e) {
    // textureInfo 取得に失敗したらその借用を共通処理で返却してから return する。
    log(`textureInfo取得失敗: ${e && e.message}`);
    releaseUnregistered(texture, 'textureInfo-throw');
    return;
  }
  // release 取得の可否は返却時にまとめて扱う（共通処理が失敗を隔離する）。
  const nt = ti && ti.handle && ti.handle.ntHandle;
  const coded = (ti && ti.codedSize) || {};
  if (stopping) {
    // 計測終了後。借用として登録せず共通処理で返す（握り潰さない）。
    stats.drops++;
    releaseUnregistered(texture, 'stopping');
    return;
  }
  if (!ti || ti.pixelFormat !== 'bgra' || !Buffer.isBuffer(nt) || nt.length !== 8) {
    if (!ti || ti.pixelFormat !== 'bgra') stats.formatMismatch++;
    else stats.missingHandle++;
    releaseUnregistered(texture, 'format-mismatch');
    return;
  }
  if (stats.firstCodedSize === null) {
    stats.firstCodedSize = { ...coded };
    log(`first codedSize=${coded.width}x${coded.height} pixelFormat=${ti.pixelFormat}`);
  }
  const hex = nt.toString('hex');
  // 生値（複製前）の集計。重複排除の鍵は main が見ている値そのものである。
  if (!handleCounts.has(hex)) handleFirstSeen.set(hex, Date.now());
  handleCounts.set(hex, (handleCounts.get(hex) || 0) + 1);

  // 新規借用を登録する前に古い保留の release を済ませ、maxHeld が上限を超えない
  // ようにする（登録後に追い出すと瞬間的に 3 枚になり maxHeld が汚れる）。
  const meta = onPaint.meta || (onPaint.meta = new Map());
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
  meta.set(borrowId, { width: coded.width, height: coded.height, ntHandle: Buffer.from(nt) });

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
    if (makeOffer(borrowId, m.width, m.height, m.ntHandle)) {
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

function onDone(borrowId, status) {
  if (borrowId === inFlight) {
    inFlight = null;
    stats.dones++;
    if (status === STATUS_OK) stats.statusOk++;
    else if (status === 2) stats.statusVerifyFailed++;
    else stats.statusCopyFailed++;
    releaseOnce(borrowId, 'done');
    // 停止中は pump しない（異常停止時は登録・pump・submit をすべて止める）。
    if (!stopping) pumpQueue(onPaint.meta || new Map());
    maybeFinish();
  } else {
    // 重複・遅延 ACK は無視する（台帳の直列化）。
    stats.staleDones++;
    log(`stale Done を無視 borrow=${borrowId} status=${status}`);
  }
}

// M2: exe からの RenderRequest をレンダラへ渡す。pose/FOV は描画要求であり、
// main プロセスは値を推測・付与しない（DESIGN §7。使用証拠は画素内の ID のみ）。
// 予約先頭バイト 0x5A は --inject-stale-record の合図であり、次フレームの描画に
// 旧 pose を混ぜさせる（harness 側フック。不一致として棄却される想定）。
function forwardM2Record(m) {
  const floats = (off, n) => Array.from({ length: n }, (_, i) => m.readFloatLE(off + i * 4));
  const rec = {
    renderId: m.readBigUInt64LE(40).toString(),
    connEpoch: m.readUInt32LE(48),
    spaceEpoch: m.readUInt32LE(52),
    leftPos: floats(56, 3),
    leftOri: floats(68, 4),
    rightPos: floats(84, 3),
    rightOri: floats(96, 4),
    leftFov: floats(112, 4),
    rightFov: floats(128, 4),
    reqWidth: m.readUInt32LE(144),
    reqHeight: m.readUInt32LE(148),
    staleHint: m[152] === 0x5A,
  };
  stats.renderRequests++;
  if (rec.staleHint) {
    log(`M2 検証フック: renderId=${rec.renderId} に旧 pose 混入の合図（次フレームで混ぜる）`);
  }
  if (winRef !== null && !winRef.isDestroyed()) {
    winRef.webContents.executeJavaScript(`window.__m2push(${JSON.stringify(rec)})`).catch(() => {});
  }
}

function maybeFinish() {
  if (!stopping || finished) return;
  if (inFlight !== null) return;  // 未 offer の保留は転送前 drop と同じ扱いで finish 側が release するため、
  // inFlight の ACK 揃いだけでソケットを閉じてよい。低速 consumer（--delay-ms）では
  // 停止時に保留が残るが、pump 停止後は offer されないため待っても解消しない。
  // 高速な通常経路では停止時に保留は空であり、従来どおりの条件になる。
  if (socket === null || socket.destroyed) return;
  log('全借用の ACK が揃ったためソケットを閉じる（未 offer の保留は終了時に返却）');
  try { socket.end(); } catch { /* 無視 */ }
}

function finish(code) {
  if (finished) return;
  finished = true;
  // 終了確定。以後の新規 paint・pump・submit を止める。
  stopping = true;
  if (stopTimer) clearTimeout(stopTimer);
  if (graceTimer) clearTimeout(graceTimer);
  if (exeWaitTimer) clearTimeout(exeWaitTimer);
  // 未 offer の保留だけ release する。offer 済み未 ACK は参照を held から外さず
  // quarantined へ移して隔離する（release しない。参照を捨てない）。
  if (pending !== null) {
    if (onPaint.meta) onPaint.meta.delete(pending);
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
  const intervals = [];
  for (let i = 1; i < paintTimes.length; i++) intervals.push(paintTimes[i] - paintTimes[i - 1]);
  const sorted = [...intervals].sort((a, b) => a - b);
  const q = (p) => sorted.length ? sorted[Math.min(sorted.length - 1, Math.floor(p * sorted.length))] : 0;
  const total = paintTimes.length > 1 ? paintTimes[paintTimes.length - 1] - paintTimes[0] : 0;
  stats.distinctHandles = handleCounts.size;
  // ソースハンドル（複製前の生値）の性質。毎フレーム新規か少数再利用かの判定用。
  // distinctSourceHandles は生値の種類数、sourceNewLast60s は終了時刻の直近 60 秒に
  // 初出た種類数、sourceNewPer30s は開始起点の 30 秒区切りごとの新規種類数である。
  const finishNow = Date.now();
  let sourceNewLast60s = 0;
  for (const t of handleFirstSeen.values()) {
    if (finishNow - t <= 60000) sourceNewLast60s++;
  }
  const startMs = paintTimes.length > 0 ? paintTimes[0] : finishNow;
  const nBuckets = Math.max(1, Math.ceil((finishNow - startMs) / 30000));
  const sourceNewPer30s = new Array(nBuckets).fill(0);
  for (const t of handleFirstSeen.values()) {
    const b = Math.min(nBuckets - 1, Math.max(0, Math.floor((t - startMs) / 30000)));
    sourceNewPer30s[b]++;
  }
  stats.distinctSourceHandles = handleCounts.size;
  stats.sourceNewLast60s = sourceNewLast60s;
  stats.sourceNewPer30s = sourceNewPer30s;
  const summary = {
    exe: EXE_PATH, seconds: SECONDS, window: `${WIN_W}x${WIN_H}`,
    mode: MODE, exeMemLog: EXE_MEM_LOG || null,
    gpuPidStart, gpuPid: currentGpuPid(),
    paints: paintTimes.length,
    fps: total > 0 ? (intervals.length / (total / 1000)) : 0,
    paintIntervalMs: { min: q(0), p50: q(0.5), p95: q(0.95), max: q(1) },
    distinctHandles: handleCounts.size,
    distinctSourceHandles: handleCounts.size,
    sourceNewLast60s,
    sourceNewPer30s,
    ...stats,
  };
  try {
    fs.writeFileSync(statsPath, JSON.stringify(summary, null, 2));
  } catch (e) {
    log('stats write FAILED: ' + (e && e.message));
  }
  // 終了コードの決定。verify/copy 失敗・隔離・未返却・maxHeld 超過・offer 失敗・
  // release 失敗が 1 件でもあれば非ゼロ終了にする。
  let finalCode = code;
  const unreturned = stats.offers - stats.dones;
  const failures = [];
  if (stats.statusCopyFailed > 0) failures.push(`copyFail=${stats.statusCopyFailed}`);
  if (stats.statusVerifyFailed > 0) failures.push(`verifyFail=${stats.statusVerifyFailed}`);
  if (stats.quarantined > 0) failures.push(`quarantine=${stats.quarantined}`);
  if (unreturned !== 0) failures.push(`unreturned=${unreturned}`);
  if (stats.maxHeld > MAX_HELD) failures.push(`maxHeld=${stats.maxHeld}>${MAX_HELD}`);
  if (stats.offerFailures > 0) failures.push(`offerFailures=${stats.offerFailures}`);
  if (releaseFailed) failures.push('releaseFailed');
  if (failures.length > 0) {
    if (finalCode === 0) finalCode = 2;
    log(`終了コードを非ゼロ化 (${failures.join(' ')})`);
    summary.failures = failures;
    summary.exitEscalated = true;
    try {
      fs.writeFileSync(statsPath, JSON.stringify(summary, null, 2));
    } catch { /* 無視 */ }
  }
  flushLog();
  log(`harness 終了 code=${finalCode} offers=${stats.offers} dones=${stats.dones} drops=${stats.drops} ` +
      `droppedDone=${stats.droppedDone} ` +
      `ok=${stats.statusOk} copyFail=${stats.statusCopyFailed} verifyFail=${stats.statusVerifyFailed} ` +
      `maxHeld=${stats.maxHeld} quarantine=${stats.quarantined}`);
  flushLog();
  // exe 側の最終統計の出力を待つため、正常終了を最大 3 秒待ってから kill する。
  // 既に終了していれば即座に抜ける。切断検知の直後に kill すると exe の統計が欠ける。
  const waitExe = new Promise((resolve) => {
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
  waitExe.then(() => {
    try {
      if (exeProc && !exeProc.killed && exeProc.exitCode === null) exeProc.kill();
    } catch { /* 無視 */ }
    flushLog();
    app.exit(finalCode);
  });
}

async function createWindow() {
  const win = new BrowserWindow({
    width: WIN_W,
    height: WIN_H,
    show: false,
    frame: false,
    // 画面作業領域（実測 2560x1392）への収め直しを防ぐ。M1 は 4096x2048 を要求する。
    enableLargerThanScreen: true,
    webPreferences: {
      offscreen: { useSharedTexture: true },
      contextIsolation: true,
      nodeIntegration: false,
    },
  });
  // 生成直後の境界は画面作業領域に収め直される（実測 2560x1392）。
  // setContentSize で強制すると 4096x2048 が通る。観測済みの手順である。
  win.setContentSize(WIN_W, WIN_H);
  win.webContents.setFrameRate(90);
  log(`window created: ${WIN_W}x${WIN_H} offscreen.useSharedTexture=true frameRate=90 target=${SECONDS}s`);
  // M2 のレコード転送先。RenderRequest の到着より先に設定する。
  winRef = win;

  win.webContents.on('console-message', (_e, _level, message) => {
    log('renderer-console: ' + message);
  });
  win.webContents.on('paint', (details) => {
    try {
      onPaint(details);
    } catch (e) {
      log('paint handler error: ' + (e && e.stack || e));
      // 未登録の借用が残っていれば共通処理で返却する（握り潰さない）。
      releaseOrphanFromPaint(details, 'paint-outer-catch');
    }
  });

  if (M2) {
    await win.loadFile(path.join(__dirname, 'index-m2.html'), {
      query: {
        staleRecord: String(INJECT_STALE_RECORD),
      },
    });
    log(`index-m2.html loaded (inject staleRecord=${INJECT_STALE_RECORD})`);
  } else {
    await win.loadFile(path.join(__dirname, 'index.html'), {
      query: {
        badTile: String(INJECT_BAD_TILE),
        staleTile: String(INJECT_STALE_TILE),
        badCorner: String(INJECT_BAD_CORNER),
        badBit: String(INJECT_BAD_BIT),
      },
    });
    log('index.html loaded ' +
        `(inject badTile=${INJECT_BAD_TILE} staleTile=${INJECT_STALE_TILE} ` +
        `badCorner=${INJECT_BAD_CORNER} badBit=${INJECT_BAD_BIT})`);
  }
  gpuPidStart = currentGpuPid();
  log(`GPU プロセス pid=${gpuPidStart}（app.getAppMetrics() から取得）`);

  stopTimer = setTimeout(() => {
    log(`${SECONDS}s 経過。新規 paint の受付を止める`);
    stopping = true;
    maybeFinish();
    // inFlight が残れば猶予後に隔離して終了する（timeout を返却許可にしない）。
    graceTimer = setTimeout(() => {
      log('grace 期限切れ。未 ACK を隔離して終了する');
      // 未 ACK が残る猶予切れは隔離（失敗）扱いのため終了 2 にする。ACK 消失の模擬
      // （--drop-done-at）の期待値である。残債が無ければ従来どおり 3。
      finish(inFlight !== null ? 2 : 3);
    }, 8000);
  }, SECONDS * 1000);
}

function start() {
  // パイプ名は electron 側の pid で作る。exe の pid を起動引数に使うことは
  // Node の spawn ではできない（引数確定より pid 取得が後になる）ため、
  // 一意性という目的を electron pid で満たす。詳細は README の既知の制約を参照。
  const pipePath = `\\\\.\\pipe\\xrift_m1_${process.pid}`;
  // M2 モードは XR 提出ループを起動する。M1 の --transfer 経路は変えない。
  const exeArgs = M2
    ? ['--xr-transfer', `--pipe=${pipePath}`, `--render-scale=${RENDER_SCALE}`,
       `--seconds=${SECONDS + 30}`]
    : ['--transfer', `--pipe=${pipePath}`, '--verify', `--seconds=${SECONDS + 30}`,
       `--mode=${MODE}`];
  if (M2) {
    log(`exe の M2 XR提出モード: render-scale=${RENDER_SCALE}`);
    if (INJECT_STALE_RECORD > 0) {
      exeArgs.push(`--inject-stale-record=${INJECT_STALE_RECORD}`);
      log(`検証フック有効: exe に --inject-stale-record=${INJECT_STALE_RECORD} を渡す ` +
          `（その要求に旧 pose 混入の合図。不一致で棄却される想定）`);
    }
    if (NO_SUBMIT_VERIFY) {
      exeArgs.push('--no-submit-verify');
      log('左右分離検証を無効化 (--no-submit-verify)');
    }
  } else {
    exeArgs.push(NO_GPU_CHECK ? '--no-gpu-check' : '--gpu-check');
    log(`exe の診断モード: ${MODE}`);
    log(`exe の GPU検査: ${NO_GPU_CHECK ? 'off (--no-gpu-check)' : 'on'}`);
  }
  if (EXE_MEM_LOG !== '') {
    exeArgs.push(`--mem-log=${EXE_MEM_LOG}`);
    log(`exe のメモリログ: ${EXE_MEM_LOG}（1 秒周期 CSV）`);
  }
  if (INJECT_BAD_TILE > 0 || INJECT_STALE_TILE > 0 || INJECT_BAD_CORNER > 0 || INJECT_BAD_BIT > 0) {
    log(`注入フック有効: badTile=${INJECT_BAD_TILE} staleTile=${INJECT_STALE_TILE} ` +
        `badCorner=${INJECT_BAD_CORNER} badBit=${INJECT_BAD_BIT}（不一致が出る想定）`);
  }
  if (FAIL_AFTER > 0) {
    exeArgs.push(`--fail-after=${FAIL_AFTER}`);
    log(`検証フック有効: exe に --fail-after=${FAIL_AFTER} を渡す`);
  }
  if (INJECT_UNCONFIRMED > 0) {
    exeArgs.push(`--inject-unconfirmed=${INJECT_UNCONFIRMED}`);
    log(`検証フック有効: exe に --inject-unconfirmed=${INJECT_UNCONFIRMED} を渡す ` +
        `（その件目は Done を送らず終了 2 の想定）`);
  }
  if (DELAY_MS > 0) {
    exeArgs.push(`--delay-ms=${DELAY_MS}`);
    log(`検証フック有効: exe に --delay-ms=${DELAY_MS} を渡す（consumer 遅延の模擬）`);
  }
  if (DROP_DONE_AT > 0) {
    log(`検証フック有効: --drop-done-at=${DROP_DONE_AT}（その個目の Done を捨てる。ACK 消失の模擬）`);
  }
  exeProc = spawn(EXE_PATH, exeArgs, {
    stdio: ['ignore', 'inherit', 'inherit'],
  });
  log(`exe spawned: pid=${exeProc.pid} pipe=${pipePath}`);
  exeProc.on('error', (e) => {
    log(`exe spawn error: ${e && e.message}`);
    finish(1);
  });
  exeProc.on('exit', (code, signal) => {
    exeCode = (typeof code === 'number') ? code : 2;
    log(`exe exit: code=${code} signal=${signal}`);
    if (exeWaitTimer) {
      clearTimeout(exeWaitTimer);
      exeWaitTimer = null;
    }
    // パイプが既に無い（未接続死・close 済み）場合はコード確定。接続中は close 側の
    // 終了待ちに確定を委ねる（close→finish(0) 確定の競合をなくす）。
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
    s.on('data', (chunk) => {
      acc = Buffer.concat([acc, chunk]);
      while (acc.length >= MSG_LEN) {
        const m = acc.subarray(0, MSG_LEN);
        acc = acc.subarray(MSG_LEN);
        if (m.readUInt32LE(0) !== MAGIC || m.readUInt16LE(4) !== VERSION) {
          log('不正なメッセージを破棄（magic/version の不一致）');
          continue;
        }
        const msgType = m.readUInt16LE(6);
        // M2: exe→main の描画要求。借用台帳には触れずレンダラへ渡す。
        if (msgType === TYPE_RENDER_REQUEST) {
          forwardM2Record(m);
          continue;
        }
        if (msgType !== TYPE_DONE) {
          log(`未知のメッセージ種別を破棄 type=${msgType}`);
          continue;
        }
        const borrowId = Number(m.readBigUInt64LE(8));
        const status = m.readUInt32LE(36);
        doneSeq++;
        // 検証用の ACK 消失フック（--drop-done-at=N）。N 個目の Done を捨てる。
        // 捨てた借用は未 ACK のまま残り、以降のフレームは処理中 1 + 保留 1 の範囲で
        // drop される。終了時に未 ACK は quarantined に入り release されない。
        // 既定 0 は捨てない。
        if (DROP_DONE_AT > 0 && doneSeq === DROP_DONE_AT) {
          stats.droppedDone++;
          log(`検証フック --drop-done-at=${DROP_DONE_AT}: Done#${doneSeq} を捨てる ` +
              `(borrow=${borrowId} status=${status}。未 ACK のまま残す)`);
          continue;
        }
        onDone(borrowId, status);
      }
    });
    s.on('close', () => {
      log('パイプ切断を検知');
      socket = null;
      pipeClosed = true;
      if (exeCode !== null) {
        // exe は既に終了している。exe のコードで確定する。
        finish(exeBaseCode());
      } else {
        // exe の終了を待ってから確定する（タイムアウト付き）。close 時点で
        // finish(0) を確定し、後の exe 非ゼロを無視する競合をなくす。
        log('exe の終了待ちを開始（上限 5 秒）');
        exeWaitTimer = setTimeout(() => {
          exeWaitTimer = null;
          log('exe 終了待ちがタイムアウト。非ゼロ扱いで終了する');
          finish(2);
        }, EXE_WAIT_MS);
      }
    });
    s.on('error', (e) => {
      log(`パイプエラー: ${e && e.message}`);
    });
  });
  server.on('error', (e) => {
    log(`パイプサーバ失敗: ${e && e.message}`);
    finish(1);
  });
  server.listen(pipePath, () => {
    log(`pipe server listening: ${pipePath}`);
  });

  createWindow().catch((e) => {
    log('createWindow FAILED: ' + (e && e.stack || e));
    finish(1);
  });
}

app.whenReady().then(() => {
  log(`electron=${process.versions.electron} chrome=${process.versions.chrome} node=${process.versions.node}`);
  // 終了保険（計測 + 猶予 + 余裕）。EPIPE 同様、保険発火も finish 経由で台帳を守る。
  setTimeout(() => {
    log(`HARD TIMEOUT 発火`);
    finish(2);
  }, (SECONDS + 60) * 1000);
  start();
});
app.on('window-all-closed', () => { /* 何もしない（paint 収集中は閉じない） */ });
