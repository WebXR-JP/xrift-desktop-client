// M1 検証ハーネスの main プロセス。検証用であり本番 TS ではない。
// 役割: パイプサーバ + 借用台帳 + release 専任。提出には関与しない。
//  - BrowserWindow（show:false、offscreen useSharedTexture）で paint を受ける
//  - 借用は「処理中 1 枚 + 保留 1 枚」まで。超過した古い保留は即 release する
//  - Done を受けたらその借用だけ release する（二重 release しない）
//  - exe の終了・切断時は、未 offer の保留だけ release する。offer 済み未 ACK は
//    GPU 完了の証拠が無いため release せず隔離し（DESIGN §6②）、Electron 終了で回収する
'use strict';
const { app, BrowserWindow } = require('electron');
const { spawn, spawnSync } = require('node:child_process');
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
// M3 障害注入フック（既定は全て 0=off。通常経路の挙動は変えない）。
// harness 側: --inject-release-fail=N（N 回目の texture.release() を例外にする。
// 実際に release せず投げ、Electron 側の解放失敗を模擬する）、
// --inject-renderer-crash=Ms（起動から Ms 後に renderer を強制クラッシュさせる）、
// --inject-gpu-kill=Ms（起動から Ms 後に GPU プロセスを kill する）。
// いずれもテスト専用であり、通常経路のコードを分岐で汚さないよう注入点は
// doReleaseTexture・クラッシュ/ kill スケジューラの各 1 箇所にまとめる。
const INJECT_RELEASE_FAIL = Math.max(0, parseInt(arg('inject-release-fail', '0'), 10) || 0);
const INJECT_RENDERER_CRASH_MS = Math.max(0, parseInt(arg('inject-renderer-crash', '0'), 10) || 0);
const INJECT_GPU_KILL_MS = Math.max(0, parseInt(arg('inject-gpu-kill', '0'), 10) || 0);
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

// harness の使い方（--help で表示。注入は既定 off。通常経路の挙動は変えない）。
if (process.argv.includes('--help') || process.argv.includes('-h')) {
  console.log(
    '使い方: electron . --exe=<path> --seconds=N --width=W --height=H [--mode=vulkan|d3d-open-only|no-gpu]\n' +
    '  障害注入（いずれも既定 0=無効。--help / README に明記する範囲だけ）:\n' +
    '  --fail-after=N … exe へ渡す人為的非ゼロ終了フック\n' +
    '  --inject-unconfirmed=N … exe へ渡す完了未確認フック（その件目は Done なし）\n' +
    '  --delay-ms=N … exe へ渡す consumer 遅延フック\n' +
    '  --drop-done-at=N … N 個目の Done を harness 側で捨てる（ACK 消失の模擬）\n' +
    '  --inject-release-fail=N … N 回目の texture.release() を例外にする（解放失敗の模擬）。\n' +
    '    参照を隔離に保持し、以降の新規 Offer を止め、非ゼロ終了する。二重 release はしない\n' +
    '  --inject-renderer-crash=Ms … 起動から Ms 後に renderer を強制クラッシュさせる。\n' +
    '    ハングせず停止し、未 ACK を隔離し、非ゼロ終了する。exe は切断で終了する\n' +
    '  --inject-gpu-kill=Ms … 起動から Ms 後に GPU プロセスを kill する。ハングしないことが必須。\n' +
    '    Chromium が GPU を再起動する場合はその挙動を記録し、そうでなければ停止・非ゼロ終了する\n' +
    '  M4（--m2 時のみ）:\n' +
    '  --space=<auto|local|stage|local-floor> … exe の参照空間の要求（既定 auto）。\n' +
    '    不可なら落とさず後退し、理由は exe のログに出る\n' +
    '  --haptic-at=Ms[,Ms...] … 起動から Ms 後に HapticCommand を送る（既定 0=無効。\n' +
    '    カンマ区切りで複数時刻を指定できる。左手・振幅 0.8・300ms。\n' +
    '    inactive・失敗は exe 側の統計に残る）\n' +
    '  --haptic-repeat=N … 5 秒間隔で N 回送る（既定 0=無効。左右交互・振幅 1.0・500ms）\n' +
    '  --inject-space-change=Ms … exe に渡す模擬 space 変更フック（既定 0=無効。\n' +
    '    実イベントではないことを exe 側のログに明記し、統計の space変更に計上する）\n' +
    '  --inject-session-restart=Ms … exe に渡す意図的セッション再生成フック（既定 0=無効）\n' +
    '  --dump-submit=<dir> [--dump-frame=N] … exe に渡す提出画像ダンプ（既定無効。N 既定 60）。\n' +
    '    N 回目の提出時に両眼 swapchain を PPM 保存する。製品経路の挙動は変えない\n' +
    '  --guide[=1|0] … HMD 操作ガイドの HTML オーバーレイ（既定 --m2 時 on、それ以外 off）。\n' +
    '    canvas 描画に触らず Chromium 合成で乗るだけのため検査結果は変わらない\n' +
    '  --guide-plan=default|short … ガイドの手順プラン（既定 default。short は 40 秒用 13 ステップ）。\n' +
    '    --m2 時のみ有効。通常経路の挙動は変えない\n' +
    '  --capture-at=Ms,path … 起動から Ms 後に webContents.capturePage() で PNG を保存する。\n' +
    '    検証専用（描画経路に入れない）。複数は ; 区切り');
  app.whenReady().then(() => app.exit(0));
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
// 検証用の提出画像ダンプ（exe の --dump-submit/--dump-frame へ渡す。既定 ''=無効）。
const DUMP_SUBMIT = arg('dump-submit', '');
const DUMP_FRAME = Math.max(1, parseInt(arg('dump-frame', '60'), 10) || 60);
// M4: 参照空間の要求（exe の --space へ渡す。既定 auto）。
const SPACE = arg('space', 'auto');
// M4: haptics 指令の送信時刻（起動から Ms 後に送る。既定 0=無効）。
// カンマ区切りで複数時刻を指定できる（例: --haptic-at=5000,15000,25000）。
// 各送信は左手・振幅 0.8・300ms。inactive・失敗は exe 側の統計に残る。
const HAPTIC_AT_RAW = arg('haptic-at', '0');
const HAPTIC_AT_LIST = HAPTIC_AT_RAW.split(',')
  .map((s) => Math.max(0, parseInt(s.trim(), 10) || 0))
  .filter((v) => v > 0);
// M4: haptics の繰り返し送信（5 秒間隔で N 回。既定 0=無効）。
// 左右交互・振幅 1.0・500ms。実機の体感再確認用であり、通常経路の挙動は変えない。
const HAPTIC_REPEAT = Math.max(0, parseInt(arg('haptic-repeat', '0'), 10) || 0);
// M4: 模擬の space 変更（exe の --inject-space-change へ渡す。既定 0=無効）。
const INJECT_SPACE_CHANGE_MS = Math.max(0, parseInt(arg('inject-space-change', '0'), 10) || 0);
// M4: 意図的セッション再生成の検証フック（exe の --inject-session-restart へ渡す。既定 0=無効）。
const INJECT_SESSION_RESTART_MS = Math.max(0, parseInt(arg('inject-session-restart', '0'), 10) || 0);
// HMD 操作ガイド（index-m2.html の HTML/CSS オーバーレイ）。--guide / --guide=1 で
// 有効、--guide=0 / --no-guide で無効。既定は M2 実機計測で on、他では off であり、
// 既存の検証（off のまま）に影響しない。canvas 描画には触らない。
const GUIDE = (() => {
  const raw = arg('guide', '');
  if (raw !== '') {
    const v = raw.trim().toLowerCase();
    if (['0', 'false', 'off', 'no'].includes(v)) return false;
    return true;
  }
  if (process.argv.includes('--guide')) return true;
  if (process.argv.includes('--no-guide')) return false;
  return M2;
})();
// HMD 操作ガイドの手順プラン（index-m2.html の ?guidePlan へ渡す。既定 default）。
// short は 40 秒用の 13 ステップ。--m2 時のみ有効であり、通常経路の挙動は変えない。
const GUIDE_PLAN = arg('guide-plan', 'default');
if (!['default', 'short'].includes(GUIDE_PLAN)) {
  console.error(`unknown --guide-plan=${GUIDE_PLAN}（default / short のいずれか）`);
  process.exitCode = 1;
  app.whenReady().then(() => app.exit(1));
  return;
}
// 検証専用の画面 capture（--capture-at=Ms,path。複数は ; 区切り）。
// 指定時刻に webContents.capturePage() で PNG を保存する。描画経路には入れない。
const CAPTURE_AT_LIST = (() => {
  const raw = arg('capture-at', '');
  if (raw === '') return [];
  return raw.split(';').map((s) => s.trim()).filter((s) => s !== '').map((s) => {
    const i = s.indexOf(',');
    if (i <= 0) return null;
    const ms = Math.max(0, parseInt(s.slice(0, i).trim(), 10) || 0);
    const file = s.slice(i + 1).trim();
    if (!(ms > 0) || file === '') return null;
    return { ms, file };
  }).filter((v) => v !== null);
})();
if (!['auto', 'local', 'stage', 'local-floor'].includes(SPACE)) {
  console.error(`unknown --space=${SPACE}（auto / local / stage / local-floor のいずれか）`);
  process.exitCode = 1;
  app.whenReady().then(() => app.exit(1));
  return;
}

// プロトコル（native_new/src/transfer/protocol.h と同じ配置。256 バイト LE）。
// M2 で 64B→192B 化し、M5 で 192B→256B 化した。先頭 64B の配置は M1 と同じである。
const MSG_LEN = 256;
const MAGIC = 0x54465258;
const VERSION = 3;
const TYPE_OFFER = 1;
const TYPE_DONE = 2;
const TYPE_RENDER_REQUEST = 3;
const TYPE_HAPTIC_COMMAND = 4; // main → exe。haptics 指令（M4）。
const TYPE_INPUT_STATE = 5;    // exe → main。入力スナップショット（M4）。
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
  inputStates: 0, // M4: exe から受けた InputState の件数。
  lastInput: null, // M4: 最後に受けた InputState の要約（統計用）。
  hapticSent: 0, // M4: exe へ送った HapticCommand の件数（--haptic-at 用）。
  captures: 0, // 検証専用の --capture-at で保存した PNG の枚数（描画経路に影響しない）。
  releaseAttempts: 0, // M3: release() の試行回数（注入判定の分母）。
  warmupDrops: 0, // M3: パターン描画の合図前の起動過渡 drop 数（転送前 drop。証拠用）。
  rendererCrashed: 0, // M3: renderer 異常を検知したら 1。
  gpuKillRequested: 0, // M3: --inject-gpu-kill の kill 信号を送ったら 1。
  gpuKilledPid: null, // M3: kill 信号を送った GPU プロセスの pid。
  gpuPidAfterKill: null, // M3: kill の約 2 秒後に再取得した GPU プロセスの pid。
  gpuKillMethod: null, // M3: GPU kill に使った手段（taskkill /F）。
  gpuKillExitCode: null, // M3: taskkill の終了コード（0 が成功の目安）。
  gpuKillPidGone: 0, // M3: kill 直後に OS 上で対象 pid が消えていたら 1。
  gpuKillPidGoneAfterWait: null, // M3: kill 約 5 秒後の再確認で消えていれば 1。
  gpuPaintsAtKill: null, // M3: kill 時点の paint 累積数（kill 後も描画継続かの分母）。
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
// M3 の異常検知状態。renderer 異常は検知したら非ゼロ終了にする（未 ACK があれば隔離）。
// GPU kill は kill の成否と kill 後の pid を記録する（回復の有無の判定材料）。
// いずれも既定 off であり、通常経路の挙動は変えない。
let rendererCrashed = false;
let rendererCrashTimer = null;
let hapticTimers = []; // M4 の --haptic-at/--haptic-repeat 用（複数回送る）。
let captureTimers = []; // 検証専用の --capture-at 用（描画経路に入れない）。
let gpuKillTimer = null;
let gpuKillObserveTimer = null;
// M3 の描画準備ゲート。M1 はレンダラの最初のパターン描画の合図
// （'[m1] pattern-live'）まで Offer しない。起動過渡の黒 paint を渡すと
// exe の検査が全面不一致で落とす（起動過渡の混入であり破損ではない）。
// 転送前 drop として即返却する。M2 は合図を出さないため最初から true。
let patternLive = M2;
// 終了コード確定の状態。パイプ close で finish(0) を確定し、後の exe 非ゼロを
// 無視する競合をなくすため、close → exe 終了待ち（タイムアウト付き）→ 確定の順にする。
let exeCode = null; // exe の終了コード（signal 死は非ゼロ扱い）。未終了は null。
let pipeClosed = false;
let exeWaitTimer = null;
const EXE_WAIT_MS = 5000; // パイプ close 後の exe 終了待ち上限。
function exeBaseCode() { return exeCode === 0 ? 0 : 2; }

// M3 テスト専用の注入点（1 箇所に集約。通常経路のコードを分岐で汚さない）。
// N 回目の release() 呼び出しを例外にし、Electron 側の解放失敗を模擬する。
// 実際に release せず投げるため、呼び出し側は隔離・世代終了の扱いにする。
// 既定 0 は投げない（通常経路の挙動は変えない）。
let releaseAttempts = 0;
function doReleaseTexture(texture) {
  releaseAttempts++;
  if (INJECT_RELEASE_FAIL > 0 && releaseAttempts === INJECT_RELEASE_FAIL) {
    throw new Error(`injected release failure at attempt ${releaseAttempts}`);
  }
  texture.release();
}

function releaseOnce(borrowId, why) {
  const tex = held.get(borrowId);
  if (tex === undefined) return false;
  try {
    doReleaseTexture(tex);
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
// M3 の --inject-release-fail は doReleaseTexture の 1 箇所で投げる（ここでは分岐しない）。
function releaseUnregistered(texture, why) {
  try {
    doReleaseTexture(texture);
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

// M3 テスト専用の注入点（renderer 強制クラッシュの予約。1 箇所に集約）。
// 起動から Ms 後に webContents.forcefullyCrashRenderer() を呼ぶ。既定 0 は予約しない。
function scheduleRendererCrash(win) {
  if (INJECT_RENDERER_CRASH_MS <= 0) return;
  log(`検証フック有効: --inject-renderer-crash=${INJECT_RENDERER_CRASH_MS}（起動からその ms 後に renderer を強制クラッシュさせる）`);
  rendererCrashTimer = setTimeout(() => {
    rendererCrashTimer = null;
    try {
      log('検証フック --inject-renderer-crash: renderer を強制クラッシュさせる');
      win.webContents.forcefullyCrashRenderer();
    } catch (e) {
      log(`renderer の強制クラッシュに失敗: ${e && e.message}`);
    }
  }, INJECT_RENDERER_CRASH_MS);
}

// M3 の renderer 異常検知（render-process-gone）。新規受付を止め、未 ACK が残れば
// 猶予後に隔離して終了する（timeout を返却許可にしない）。paint 停止後の計測終了待ちで
// ハングしないよう、計測タイマーを前倒しして猶予へ移す。
function onRendererGone(details) {
  if (rendererCrashed) return;
  rendererCrashed = true;
  stats.rendererCrashed = 1;
  stopping = true;
  log(`renderer 異常を検知 reason=${details && details.reason} exitCode=${details && details.exitCode}（新規受付を止める）`);
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
}

// OS 上に pid が存在するか（tasklist 照会。ASCII の pid 部分だけ見るため
// 日本語ロケールの出力を UTF-8 誤読しても判定に影響しない）。
// 照会自体に失敗した場合は存在扱いで安全側に倒す（誤って成功扱いにしない）。
function pidExistsOnWindows(pid) {
  try {
    const r = spawnSync('tasklist.exe', ['/FI', `PID eq ${pid}`, '/NH', '/FO', 'CSV'], { encoding: 'utf8' });
    if (r.error) return true;
    return String(r.stdout || '').includes(`"${pid}"`);
  } catch {
    return true;
  }
}

// M3 テスト専用の注入点（GPU プロセス kill の予約。1 箇所に集約）。
// 起動から Ms 後に app.getAppMetrics() の type=gpu の pid を taskkill /F で
// 強制終了する。kill 直後に OS 上の pid 消滅を確認し、約 5 秒間の pid 再取得で
// Chromium が GPU を再起動したかを記録する。
// 既定 0 は予約しない。ハングしないことが必須であり、回復の有無で終了の扱いを分ける
// （回復時はその旨を記録し、マトリクス側で合否を分ける）。
// ADR: 以前は process.kill(pid) を使っていたが GPU プロセスが落ちないことを確認済み
// （kill 前後で pid 不変・エラーなし）。Windows の taskkill /F が確実に落とす。
function scheduleGpuKill() {
  if (INJECT_GPU_KILL_MS <= 0) return;
  log(`検証フック有効: --inject-gpu-kill=${INJECT_GPU_KILL_MS}（起動からその ms 後に GPU プロセスを kill する）`);
  gpuKillTimer = setTimeout(() => {
    gpuKillTimer = null;
    const pid = currentGpuPid() !== null ? currentGpuPid() : gpuPidStart;
    if (pid === null || pid === undefined) {
      log('検証フック --inject-gpu-kill: GPU プロセスの pid が取れないため kill できない');
      return;
    }
    stats.gpuKillRequested = 1;
    stats.gpuKilledPid = pid;
    stats.gpuKillMethod = 'taskkill /F';
    log(`検証フック --inject-gpu-kill: GPU プロセス pid=${pid} を taskkill /F で終了させる`);
    try {
      // 出力はロケール文字化けを避けて捨て、終了コードだけを証拠にする。
      // pid の消滅自体は pidExistsOnWindows と kill 後の pid 再取得で確認する。
      const r = spawnSync('taskkill.exe', ['/F', '/PID', String(pid)], { stdio: 'ignore' });
      stats.gpuKillExitCode = r.status;
      log(`taskkill /F /PID ${pid} 終了 code=${r.status}`);
      if (r.error) {
        log(`taskkill の起動に失敗 pid=${pid}: ${r.error && r.error.message}`);
        return;
      }
    } catch (e) {
      log(`GPU プロセスの kill に失敗 pid=${pid}: ${e && e.message}`);
      return;
    }
    const gone = !pidExistsOnWindows(pid);
    stats.gpuKillPidGone = gone ? 1 : 0;
    stats.gpuPaintsAtKill = paintTimes.length;
    log(gone
      ? `GPU プロセス pid=${pid} の消滅を確認（OS 上に存在しない）`
      : `GPU プロセス pid=${pid} が OS 上に残っている（taskkill code=${stats.gpuKillExitCode}）`);
    // kill 後約 5 秒間、1 秒ごとに GPU pid を再取得する。
    // getAppMetrics が死んだ pid を返し続ける場合があるため、単発の一致では
    // 生死を判断しない。OS 上の pid 消滅と終了時の gpu 有無を合わせて証拠にする。
    const killPollStart = Date.now();
    gpuKillObserveTimer = setInterval(() => {
      const polled = currentGpuPid();
      log(`GPU kill 後の観測 pid=${polled}（kill 前=${pid}）`);
      if (Date.now() - killPollStart >= 5000) {
        clearInterval(gpuKillObserveTimer);
        gpuKillObserveTimer = null;
        stats.gpuPidAfterKill = polled;
        const goneAfter = !pidExistsOnWindows(pid);
        stats.gpuKillPidGoneAfterWait = goneAfter ? 1 : 0;
        log(`GPU kill 観測終了 pid=${polled}（kill 前=${pid}）。OS 上の消滅=${goneAfter ? '確認' : '残存'}`);
      }
    }, 1000);
  }, INJECT_GPU_KILL_MS);
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
  // M3 の描画準備ゲート（M1 のみ）。合図前の paint は転送前 drop として即返却する。
  if (!patternLive) {
    stats.drops++;
    stats.warmupDrops++;
    releaseUnregistered(texture, 'warming-up');
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

// M4: exe からの InputState を読む。M5 で左右の grip/aim pose（position xyz＋
// orientation xyzw の float LE。左 grip@88・左 aim@116・右 grip@160・右 aim@188）
// を足した。有効性は手 flags（bit1=grip・bit2=aim）が表す。
// 「0」と「未対応」の区別は active/supported の 2 段で行う。1 秒に 1 回だけ要約をログに出す。
// 予約先頭 2B（offset 152,153）は左右の押下ラッチ（その 1 秒間に押された
// ボタンの集合。buttons と同じ bit 割付け。protocol.h を参照）。
const PROFILE_NAMES = {
  0: '-', 1: 'simple', 2: 'oculus', 3: 'index', 4: 'vive', 5: 'motion',
  6: 'pico4', 7: 'pico-neo3', 8: 'pico-ultra', 9: 'pico-g3', 255: 'other',
};
const LATCH_BUTTON_NAMES = ['stick', 'pad', 'primary', 'secondary', 'menu'];
function decodeLatch(bits) {
  const names = [];
  for (let b = 0; b < 5; b++) {
    if ((bits & (1 << b)) !== 0) names.push(LATCH_BUTTON_NAMES[b]);
  }
  return names.length ? names.join(',') : '-';
}
let lastInputLogAt = 0;
function decodeInputHand(m, base) {
  const flags = m[base];
  const btns = ['stick', 'pad', 'primary', 'secondary', 'menu'];
  const pressed = [];
  const bits = m[base + 8];
  for (let b = 0; b < 5; b++) {
    if ((bits & (1 << b)) !== 0) pressed.push(btns[b]);
  }
  const stickActive = (m[base + 11] & 1) !== 0;
  return {
    connected: (flags & 1) !== 0,
    gripValid: (flags & 2) !== 0,
    aimValid: (flags & 4) !== 0,
    profile: PROFILE_NAMES[m[base + 1]] || `id=${m[base + 1]}`,
    trigger: m[base + 2] / 255,
    squeeze: m[base + 3] / 255,
    stick: [m.readInt8(base + 4) / 127, m.readInt8(base + 5) / 127],
    pad: [m.readInt8(base + 6) / 127, m.readInt8(base + 7) / 127],
    buttons: pressed.length ? pressed.join(',') : '-',
    btnActive: m[base + 9],
    floatActive: m[base + 10],
    vecActive: m[base + 11],
    btnSupported: m[base + 12],
    analogSupported: m[base + 13],
    stickState: stickActive ? '' : (((m[base + 13] & 4) !== 0) ? '(inactive)' : '(非対応)'),
  };
}
// M5 の pose 1 件（position xyz＋orientation xyzw の float LE）。無効時は 0 埋め。
function decodeInputPose(m, off, valid) {
  if (!valid) return { valid: false, position: [0, 0, 0], orientation: [0, 0, 0, 0] };
  return {
    valid: true,
    position: [m.readFloatLE(off), m.readFloatLE(off + 4), m.readFloatLE(off + 8)],
    orientation: [m.readFloatLE(off + 12), m.readFloatLE(off + 16), m.readFloatLE(off + 20), m.readFloatLE(off + 24)],
  };
}
function onInputState(m) {
  const st = {
    frame: m.readBigUInt64LE(40).toString(),
    connEpoch: m.readUInt32LE(48),
    spaceEpoch: m.readUInt32LE(52),
    left: decodeInputHand(m, 56),
    right: decodeInputHand(m, 72),
    latchLeft: decodeLatch(m[152]),
    latchRight: decodeLatch(m[153]),
  };
  // M5: コントローラ pose（左 grip@88・左 aim@116・右 grip@160・右 aim@188）。
  st.left.grip = decodeInputPose(m, 88, st.left.gripValid);
  st.left.aim = decodeInputPose(m, 116, st.left.aimValid);
  st.right.grip = decodeInputPose(m, 160, st.right.gripValid);
  st.right.aim = decodeInputPose(m, 188, st.right.aimValid);
  stats.inputStates++;
  stats.lastInput = st;
  const now = Date.now();
  if (now - lastInputLogAt >= 1000) {
    lastInputLogAt = now;
    const poseText = (p) => (p.valid ? `(${p.position.map((v) => v.toFixed(2)).join(',')})` : '無効');
    const fmt = (h) => `${h.connected ? '接続' : '未接続'} prof=${h.profile} ` +
      `trig=${h.trigger.toFixed(2)} sq=${h.squeeze.toFixed(2)} ` +
      `stick=(${h.stick[0].toFixed(2)},${h.stick[1].toFixed(2)})${h.stickState} btn=${h.buttons} ` +
      `grip=${poseText(h.grip)} aim=${poseText(h.aim)}`;
    log(`[M4入力] L: ${fmt(st.left)} | R: ${fmt(st.right)} ` +
      `(frame=${st.frame} conn=${st.connEpoch} space=${st.spaceEpoch} ` +
      `latch=L:${st.latchLeft} R:${st.latchRight})`);
  }
}

// M4: HapticCommand を exe へ送る（配置は protocol.h の MakeHapticCommand と同じ）。
// hand@40、amplitude@56、durationMs@48。送信結果（成功・inactive・失敗）は
// exe 側のログと統計に残る（XrResult で記録する）。
function sendHaptic(hand, amplitude, durationMs, why) {
  if (socket === null || socket.destroyed) {
    log(`haptics 未送信: パイプが無い（${why}）`);
    return;
  }
  const buf = Buffer.alloc(MSG_LEN);
  buf.writeUInt32LE(MAGIC, 0);
  buf.writeUInt16LE(VERSION, 4);
  buf.writeUInt16LE(TYPE_HAPTIC_COMMAND, 6);
  buf.writeUInt32LE(hand, 40);
  buf.writeFloatLE(amplitude, 56);
  buf.writeUInt32LE(durationMs, 48);
  try {
    socket.write(buf);
    stats.hapticSent++;
    log(`haptics 指令を送信（${hand === 0 ? '左' : '右'}・振幅 ${amplitude}・${durationMs}ms。` +
      `inactive・失敗は exe 側の統計に残る。${why}）`);
  } catch (e) {
    log(`haptics 送信に失敗: ${e && e.message}`);
  }
}
// --haptic-at（複数時刻。各送信は左手・振幅 0.8・300ms）と
// --haptic-repeat（5 秒間隔で N 回。左右交互・振幅 1.0・500ms）の予約。
// いずれも既定は送らない（通常経路の挙動は変えない）。
function scheduleHaptics() {
  for (const at of HAPTIC_AT_LIST) {
    log(`検証フック有効: --haptic-at=${at}（起動からその ms 後に HapticCommand を送る）`);
    hapticTimers.push(setTimeout(() => {
      sendHaptic(0, 0.8, 300, `--haptic-at=${at}`);
    }, at));
  }
  if (HAPTIC_REPEAT > 0) {
    log(`検証フック有効: --haptic-repeat=${HAPTIC_REPEAT}（5 秒間隔で ${HAPTIC_REPEAT} 回送る。` +
      `左右交互・振幅 1.0・500ms）`);
    for (let i = 1; i <= HAPTIC_REPEAT; i++) {
      const hand = (i % 2 === 1) ? 0 : 1; // 奇数回=左、偶数回=右。
      hapticTimers.push(setTimeout(() => {
        sendHaptic(hand, 1.0, 500, `--haptic-repeat（${i}/${HAPTIC_REPEAT}）`);
      }, i * 5000));
    }
  }
}

// 検証専用の画面 capture（--capture-at=Ms,path）。指定時刻に
// webContents.capturePage() で PNG を保存する。描画経路（paint/Offer/検査）には
// 入れず、Chromium 合成後の見た目（ガイド表示の確認用）だけを取る。
function scheduleCapture(win) {
  for (const c of CAPTURE_AT_LIST) {
    log(`検証フック有効: --capture-at=${c.ms},${c.file}（起動からその ms 後に capturePage で PNG 保存）`);
    captureTimers.push(setTimeout(async () => {
      try {
        if (win.isDestroyed()) {
          log(`capture スキップ（window 破棄済み）: ${c.file}`);
          return;
        }
        const img = await win.webContents.capturePage();
        const out = path.isAbsolute(c.file) ? c.file : path.join(outDir, c.file);
        const dir = path.dirname(out);
        if (dir !== '' && !fs.existsSync(dir)) fs.mkdirSync(dir, { recursive: true });
        fs.writeFileSync(out, img.toPNG());
        stats.captures = (stats.captures || 0) + 1;
        log(`capture 保存: ${out}（${c.ms}ms 時点）`);
      } catch (e) {
        log(`capture 失敗 ${c.file}: ${e && e.message}`);
      }
    }, c.ms));
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
  if (rendererCrashTimer) {
    clearTimeout(rendererCrashTimer);
    rendererCrashTimer = null;
  }
  for (const t of hapticTimers) {
    clearTimeout(t);
  }
  hapticTimers = [];
  for (const t of captureTimers) {
    clearTimeout(t);
  }
  captureTimers = [];
  if (gpuKillTimer) {
    clearTimeout(gpuKillTimer);
    gpuKillTimer = null;
  }
  if (gpuKillObserveTimer) {
    // 観測は setInterval のため clearInterval で止める（Node では clearTimeout も
    // 効くが、意図を明確にする）。
    clearInterval(gpuKillObserveTimer);
    gpuKillObserveTimer = null;
  }
  // M2 の XR ループは Offer 待ちで空フレームを回し続けるため、読取側を開いたままの
  // 半閉鎖（socket.end のみ）では切断を検知できず kill による統計消失が起きる。
  // 終了待ちの前に破棄して切断（BROKEN_PIPE）として知らせ、exe に統計を出させる。
  // 未 ACK の借用は下で隔離し release しない（DESIGN §6②③）。
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
  stats.releaseAttempts = releaseAttempts;
  const summary = {
    exe: EXE_PATH, seconds: SECONDS, window: `${WIN_W}x${WIN_H}`,
    mode: MODE, exeMemLog: EXE_MEM_LOG || null,
    guide: GUIDE, guidePlan: GUIDE_PLAN, captureAt: CAPTURE_AT_LIST,
    injectReleaseFail: INJECT_RELEASE_FAIL,
    injectRendererCrashMs: INJECT_RENDERER_CRASH_MS,
    injectGpuKillMs: INJECT_GPU_KILL_MS,
    dropDoneAt: DROP_DONE_AT, injectUnconfirmed: INJECT_UNCONFIRMED, delayMs: DELAY_MS,
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
  if (rendererCrashed) failures.push('rendererCrashed');
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
    // M3 の描画準備ゲートの合図（M1 の index.html が最初のパターン描画後に 1 回出す）。
    if (!patternLive && typeof message === 'string' && message.startsWith('[m1] pattern-live')) {
      patternLive = true;
      log('レンダラのパターン描画を確認。以後の paint を Offer 対象にする');
    }
  });
  // M3: renderer 異常の検知。ハングせず停止し、未 ACK を隔離して非ゼロ終了する。
  win.webContents.on('render-process-gone', (_e, details) => {
    log(`render-process-gone: reason=${details && details.reason} exitCode=${details && details.exitCode}`);
    onRendererGone(details);
  });
  scheduleRendererCrash(win);
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
        guide: GUIDE ? '1' : '0',
        guidePlan: GUIDE_PLAN,
      },
    });
    log(`index-m2.html loaded (inject staleRecord=${INJECT_STALE_RECORD} guide=${GUIDE ? 'on' : 'off'} guidePlan=${GUIDE_PLAN})`);
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
  scheduleCapture(win);

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
       `--seconds=${SECONDS + 30}`, `--space=${SPACE}`]
    : ['--transfer', `--pipe=${pipePath}`, '--verify', `--seconds=${SECONDS + 30}`,
       `--mode=${MODE}`];
  if (M2) {
    log(`exe の M2 XR提出モード: render-scale=${RENDER_SCALE} space=${SPACE}`);
    log(`HMD 操作ガイド: ${GUIDE ? 'on' : 'off'} plan=${GUIDE_PLAN}（既定 M2=on。canvas 描画に触らない）`);
    if (INJECT_STALE_RECORD > 0) {
      exeArgs.push(`--inject-stale-record=${INJECT_STALE_RECORD}`);
      log(`検証フック有効: exe に --inject-stale-record=${INJECT_STALE_RECORD} を渡す ` +
          `（その要求に旧 pose 混入の合図。不一致で棄却される想定）`);
    }
    if (INJECT_SESSION_RESTART_MS > 0) {
      exeArgs.push(`--inject-session-restart=${INJECT_SESSION_RESTART_MS}`);
      log(`検証フック有効: exe に --inject-session-restart=${INJECT_SESSION_RESTART_MS} を渡す ` +
          `（その ms 後に意図的にセッションを破棄・再生成する）`);
    }
    if (INJECT_SPACE_CHANGE_MS > 0) {
      exeArgs.push(`--inject-space-change=${INJECT_SPACE_CHANGE_MS}`);
      log(`検証フック有効: exe に --inject-space-change=${INJECT_SPACE_CHANGE_MS} を渡す ` +
          `（その ms 後に模擬の space 変更を 1 回適用する。実イベントではない）`);
    }
    if (NO_SUBMIT_VERIFY) {
      exeArgs.push('--no-submit-verify');
      log('左右分離検証を無効化 (--no-submit-verify)');
    }
    if (DUMP_SUBMIT !== '') {
      exeArgs.push(`--dump-submit=${DUMP_SUBMIT}`);
      exeArgs.push(`--dump-frame=${DUMP_FRAME}`);
      log(`検証フック有効: exe に --dump-submit=${DUMP_SUBMIT} --dump-frame=${DUMP_FRAME} を渡す ` +
          `（その回目の提出時に両眼を PPM 保存）`);
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
  if (INJECT_RELEASE_FAIL > 0) {
    log(`検証フック有効: --inject-release-fail=${INJECT_RELEASE_FAIL}（その回目の release() を例外にする。解放失敗の模擬）`);
  }
  // M3 の renderer クラッシュ・GPU kill の予約は createWindow 後に行う。
  // renderer 側は window が要るため createWindow 側で予約し、GPU 側はここで予約する。
  scheduleGpuKill();
  // M4 の haptics 指令（--haptic-at/--haptic-repeat。M2 モード以外では送り先が無いため M2 限定）。
  if (M2) scheduleHaptics();
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
        // M4: exe→main の入力スナップショット。台帳には触れず記録する。
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
      if (finished) return;
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
