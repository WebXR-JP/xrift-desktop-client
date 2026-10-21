// 新アプリ（M5）を SteamVR のライブラリに登録する。
//
//   node scripts/register-steamvr.cjs add     登録（SteamVR を起動しておくこと）
//   node scripts/register-steamvr.cjs check   登録確認（IsApplicationInstalled を返す）
//   node scripts/register-steamvr.cjs remove  登録解除
//   node scripts/register-steamvr.cjs add --packaged            配布版を登録
//   node scripts/register-steamvr.cjs check --packaged          配布版の登録確認
//   node scripts/register-steamvr.cjs remove --packaged         配布版の登録解除
//   node scripts/register-steamvr.cjs add --packaged=<exeのパス>  配布版の exe を明示
//
// `--packaged`（引数なし）は dist-package/XRiftClient-win32-x64/XRiftClient.exe を
// 自動検出する。配布版のマニフェストは xrift-packaged.vrmanifest に分け、
// binary は XRiftClient.exe の絶対パス、arguments は本番 URL とする。
// app_key は開発版と同じ net.xrift.client のため、add 時に既存の登録を置き換える
// （開発版と配布版は排他）。check は app_key のため両版で同じ結果を返す。
//
// transfer-addon の steamvrManifest(op, dllPath, manifestPath|appKey) を使う。
// 旧アプリ（net.xrift.openxr-client）とは app_key を変えて区別する。
'use strict';

const { existsSync, readFileSync, writeFileSync } = require('node:fs');
const { dirname, join, resolve } = require('node:path');

// client/ → XRift-Client/ → リポジトリルート相当。
const clientDir = join(__dirname, '..');
const repoRoot = join(clientDir, '..');
const APP_KEY = 'net.xrift.client';
const MANIFEST_PATH = join(clientDir, 'xrift.vrmanifest');
const PACKAGED_MANIFEST_PATH = join(clientDir, 'xrift-packaged.vrmanifest');
const ADDON_PATH = join(repoRoot, 'native', 'transfer-addon', 'build', 'Release', 'xrift_transfer_addon.node');
// 配布版の exe（--packaged 引数なし時の自動検出先）。package.cjs の出力と一致させる。
const PACKAGED_EXE_PATH = join(clientDir, 'dist-package', 'XRiftClient-win32-x64', 'XRiftClient.exe');
// 配布版の addon（resources/ 直下。package.cjs の extraResource と一致）。
// SteamVR 登録の呼び出し自体は開発版・配布版どちらの .node でも動くため、
// 配布版が無ければ開発版に退避する。
function packagedAddonPath() {
  const candidate = join(clientDir, 'dist-package', 'XRiftClient-win32-x64', 'resources', 'xrift_transfer_addon.node');
  if (existsSync(candidate)) return candidate;
  return ADDON_PATH;
}
const DEFAULT_URL = 'https://app.xrift.net/';

// argv から操作と --packaged を取り出す。順序は問わない
// （`add --packaged`・`--packaged add` のどちらでも同じ）。
// `--packaged=<exeのパス>` の形式では exe を明示できる。
function parseArgs(argv) {
  let op = 'add';
  let packaged = false;
  let packagedExe = null;
  for (const a of argv) {
    if (a === '--packaged') {
      packaged = true;
    } else if (a.startsWith('--packaged=')) {
      packaged = true;
      const v = a.slice('--packaged='.length).trim();
      if (v !== '') packagedExe = v;
    } else if (a === 'add' || a === 'check' || a === 'remove') {
      op = a;
    } else if (a === '-h' || a === '--help' || a === 'help') {
      return { op: 'help', packaged: false, packagedExe: null };
    } else {
      return { op: null, packaged: false, packagedExe: null, unknown: a };
    }
  }
  return { op, packaged, packagedExe };
}

// 配布版の exe を決める。明示パスがあればそれを使い、無ければ自動検出する。
function packagedBinary(explicit) {
  if (explicit !== null) {
    const p = resolve(explicit);
    if (!existsSync(p)) {
      throw new Error(`配布版の起動ファイルがありません: ${p}`);
    }
    return p;
  }
  if (!existsSync(PACKAGED_EXE_PATH)) {
    throw new Error(
      `配布版がありません: ${PACKAGED_EXE_PATH}（client で npm run package を実行してください）`,
    );
  }
  return PACKAGED_EXE_PATH;
}

function findSteamVR() {
  const candidates = [];
  const vrpath = join(process.env.LOCALAPPDATA ?? '', 'openvr', 'openvrpaths.vrpath');
  if (existsSync(vrpath)) {
    try {
      const parsed = JSON.parse(readFileSync(vrpath, 'utf8'));
      if (Array.isArray(parsed.runtime)) candidates.push(...parsed.runtime);
    } catch {
      // openvrpaths.vrpath が壊れていても既定の候補で探す
    }
  }
  candidates.push('C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR');
  for (const dir of candidates) {
    const dll = join(dir, 'bin', 'win64', 'openvr_api.dll');
    if (existsSync(dll)) return dll;
  }
  throw new Error(
    'SteamVR が見つかりません（openvrpaths.vrpath / 既定のインストール先に openvr_api.dll が無い）。' +
      'Steam をインストールして SteamVR を一度起動してください',
  );
}

function loadAddon(packaged) {
  const target = packaged ? packagedAddonPath() : ADDON_PATH;
  if (!existsSync(target)) {
    throw new Error(
      `transfer-addon がありません: ${target}（native/transfer-addon/build.ps1 でビルドしてください）`,
    );
  }
  const mod = require(target);
  if (mod === null || typeof mod !== 'object' || typeof mod.steamvrManifest !== 'function') {
    throw new Error('transfer-addon に steamvrManifest がありません（再ビルドしてください）');
  }
  return mod;
}

function electronBinary() {
  const binary = join(repoRoot, 'node_modules', 'electron', 'dist', 'electron.exe');
  if (!existsSync(binary)) {
    throw new Error(`起動ファイルがありません: ${binary}（root で pnpm install を実行してください）`);
  }
  return binary;
}

// 開発時のマニフェストを生成する。binary は開発用の electron.exe、
// arguments は「client ディレクトリを開く」起動引数である。
// SteamVR の vrmanifest の arguments は文字列であるため、引用符つきで結合する。
function writeManifest(manifestPath) {
  const binary = electronBinary();
  const args = [`"${clientDir}"`, `--url=${DEFAULT_URL}`];
  const manifest = {
    source: 'builtin',
    applications: [
      {
        app_key: APP_KEY,
        launch_type: 'binary',
        binary_path_windows: binary,
        arguments: args.join(' '),
        working_directory: repoRoot,
        is_dashboard_overlay: false,
        strings: {
          en_us: {
            name: 'XRift Client (new)',
            description: 'XRift new app (M5): opens app.xrift.net in VR',
          },
          ja_jp: {
            name: 'XRift Client（新アプリ）',
            description: 'XRift 新アプリ（M5）。app.xrift.net を VR で開く',
          },
        },
      },
    ],
  };
  writeFileSync(manifestPath, JSON.stringify(manifest, null, 2));
  return manifest;
}

// 配布版のマニフェストを生成する。binary は XRiftClient.exe の絶対パス、
// arguments は本番 URL のみである（client 側の既定も本番 URL のため、
// 空でも同じ挙動になるが、登録時の意図を明示するため付ける）。
// working_directory は exe のあるディレクトリ（配布物の直下）とする。
function writePackagedManifest(manifestPath, binary) {
  const manifest = {
    source: 'builtin',
    applications: [
      {
        app_key: APP_KEY,
        launch_type: 'binary',
        binary_path_windows: binary,
        arguments: `--url=${DEFAULT_URL}`,
        working_directory: dirname(binary),
        is_dashboard_overlay: false,
        strings: {
          en_us: {
            name: 'XRift Client (new)',
            description: 'XRift new app (M5): opens app.xrift.net in VR',
          },
          ja_jp: {
            name: 'XRift Client（新アプリ）',
            description: 'XRift 新アプリ（M5）。app.xrift.net を VR で開く',
          },
        },
      },
    ],
  };
  writeFileSync(manifestPath, JSON.stringify(manifest, null, 2));
  return manifest;
}

function usage() {
  console.log('使い方: node scripts/register-steamvr.cjs [add|check|remove] [--packaged[=<exeのパス>]]');
  console.log(`  add    マニフェスト生成 + SteamVR へ登録（app_key=${APP_KEY}）`);
  console.log('  check  登録確認（IsApplicationInstalled）');
  console.log('  remove 登録解除');
  console.log('  --packaged  配布版（dist-package/XRiftClient-win32-x64/XRiftClient.exe）を登録・確認・解除する');
}

function main() {
  const parsed = parseArgs(process.argv.slice(2));
  if (parsed.op === 'help') {
    usage();
    return;
  }
  if (parsed.op === null) {
    console.error(`不明な操作: ${parsed.unknown}`);
    usage();
    process.exitCode = 1;
    return;
  }
  const op = parsed.op;
  const packaged = parsed.packaged;
  // 配布版のマニフェストは開発版とファイルを分ける（開発版の内容を壊さないため）。
  // app_key は同じのため、add 時に既存の登録（開発版・配布版のいずれか）を置き換える。
  const manifestPath = packaged ? PACKAGED_MANIFEST_PATH : MANIFEST_PATH;
  let dll;
  try {
    dll = findSteamVR();
  } catch (e) {
    console.error(`失敗: ${e instanceof Error ? e.message : String(e)}`);
    process.exitCode = 1;
    return;
  }
  let addon;
  try {
    addon = loadAddon(packaged);
  } catch (e) {
    console.error(`失敗: ${e instanceof Error ? e.message : String(e)}`);
    process.exitCode = 1;
    return;
  }
  if (op === 'add') {
    let manifest;
    try {
      manifest = packaged
        ? writePackagedManifest(manifestPath, packagedBinary(parsed.packagedExe))
        : writeManifest(manifestPath);
    } catch (e) {
      console.error(`失敗: ${e instanceof Error ? e.message : String(e)}`);
      process.exitCode = 1;
      return;
    }
    console.log(`マニフェスト生成: ${manifestPath}`);
    console.log(`  app_key=${APP_KEY}`);
    console.log(`  binary=${manifest.applications[0].binary_path_windows}`);
    console.log(`  arguments=${manifest.applications[0].arguments}`);
    const r = addon.steamvrManifest('add', dll, manifestPath);
    if (!r || r.ok !== true) {
      console.error(`登録に失敗: ${r && r.error ? r.error : JSON.stringify(r)}`);
      process.exitCode = 1;
      return;
    }
    const check = addon.steamvrManifest('check', dll, APP_KEY);
    const installed = check && check.installed === true;
    console.log(`SteamVR に登録しました（${APP_KEY}${installed ? '' : ' / 確認できず'}）`);
    console.log('  SteamVR のダッシュボード → ライブラリに「XRift Client（新アプリ）」が出ます');
    return;
  }
  if (op === 'remove') {
    const target = existsSync(manifestPath) ? manifestPath : resolve(manifestPath);
    const r = addon.steamvrManifest('remove', dll, target);
    console.log(r && r.ok === true ? '登録を解除しました' : `解除に失敗: ${r && r.error ? r.error : JSON.stringify(r)}`);
    if (!r || r.ok !== true) process.exitCode = 1;
    return;
  }
  const r = addon.steamvrManifest('check', dll, APP_KEY);
  if (!r || r.ok !== true) {
    console.error(`確認に失敗: ${r && r.error ? r.error : JSON.stringify(r)}`);
    process.exitCode = 1;
    return;
  }
  console.log(`installed=${r.installed === true ? 'true' : 'false'}（app_key=${APP_KEY}）`);
}

main();
