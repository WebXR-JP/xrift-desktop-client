// 新アプリ（M5）を SteamVR のライブラリに登録する。
//
//   node scripts/register-steamvr.cjs add     登録（SteamVR を起動しておくこと）
//   node scripts/register-steamvr.cjs check   登録確認（IsApplicationInstalled を返す）
//   node scripts/register-steamvr.cjs remove  登録解除
//
// transfer-addon の steamvrManifest(op, dllPath, manifestPath|appKey) を使う。
// 旧アプリ（net.xrift.openxr-client）とは app_key を変えて区別する。
'use strict';

const { existsSync, readFileSync, writeFileSync } = require('node:fs');
const { dirname, join, resolve } = require('node:path');

// app/ → XRift-Client/ → リポジトリルート相当。
const appDir = join(__dirname, '..');
const repoRoot = join(appDir, '..');
const APP_KEY = 'net.xrift.client';
const MANIFEST_PATH = join(appDir, 'xrift.vrmanifest');
const ADDON_PATH = join(repoRoot, 'native_new', 'transfer-addon', 'build', 'Release', 'xrift_transfer_addon.node');
const DEFAULT_URL = 'https://app.xrift.net/';

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

function loadAddon() {
  if (!existsSync(ADDON_PATH)) {
    throw new Error(
      `transfer-addon がありません: ${ADDON_PATH}（native_new/transfer-addon/build.ps1 でビルドしてください）`,
    );
  }
  const mod = require(ADDON_PATH);
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
// arguments は「app ディレクトリを開く」起動引数である。
// SteamVR の vrmanifest の arguments は文字列であるため、引用符つきで結合する。
function writeManifest() {
  const binary = electronBinary();
  const args = [`"${appDir}"`, `--url=${DEFAULT_URL}`];
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
  writeFileSync(MANIFEST_PATH, JSON.stringify(manifest, null, 2));
  return manifest;
}

function usage() {
  console.log('使い方: node scripts/register-steamvr.cjs [add|check|remove]');
  console.log(`  add    マニフェスト生成 + SteamVR へ登録（app_key=${APP_KEY}）`);
  console.log('  check  登録確認（IsApplicationInstalled）');
  console.log('  remove 登録解除');
}

function main() {
  const op = process.argv[2] ?? 'add';
  if (op === '-h' || op === '--help' || op === 'help') {
    usage();
    return;
  }
  if (op !== 'add' && op !== 'check' && op !== 'remove') {
    console.error(`不明な操作: ${op}`);
    usage();
    process.exitCode = 1;
    return;
  }
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
    addon = loadAddon();
  } catch (e) {
    console.error(`失敗: ${e instanceof Error ? e.message : String(e)}`);
    process.exitCode = 1;
    return;
  }
  if (op === 'add') {
    const manifest = writeManifest();
    console.log(`マニフェスト生成: ${MANIFEST_PATH}`);
    console.log(`  app_key=${APP_KEY}`);
    console.log(`  binary=${manifest.applications[0].binary_path_windows}`);
    console.log(`  arguments=${manifest.applications[0].arguments}`);
    const r = addon.steamvrManifest('add', dll, MANIFEST_PATH);
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
    const target = existsSync(MANIFEST_PATH) ? MANIFEST_PATH : resolve(MANIFEST_PATH);
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
