// 新アプリ（M5）の配布パッケージを作る: client/dist-package/XRiftClient-win32-x64/
//
//   npm run package            client のビルド + パッケージ
//
// root の node_modules の @electron/packager を使う（client 自体に依存は無い）。
// 同梱物: client/dist、client/pages、client/package.json（本体）、
//   xrift_new.exe + xrift_transfer_addon.node（extraResource → resources/ 直下）、
//   three.module.js + three.core.js（→ resources/node_modules/three/build/。pages の importmap 解決用）。
// main は packagedResource() で resources/ 直下から解決する（開発時は従来パス）。
'use strict';

const { execFileSync } = require('node:child_process');
const { copyFileSync, existsSync, mkdirSync, readdirSync, statSync } = require('node:fs');
const { join } = require('node:path');

const clientDir = join(__dirname, '..');
const repoRoot = join(clientDir, '..');
const pkg = require(join(clientDir, 'package.json'));

const EXE_SRC = join(repoRoot, 'native', 'build', 'windows', 'Release', 'xrift_new.exe');
const ADDON_SRC = join(repoRoot, 'native', 'transfer-addon', 'build', 'Release', 'xrift_transfer_addon.node');

function fail(msg) {
  console.error(`[package] 失敗: ${msg}`);
  process.exit(1);
}

async function main() {
  // 事前ビルド（client の tsc）。dist/main.js が無ければ明確に落とす。
  const tscBin = join(repoRoot, 'node_modules', 'typescript', 'bin', 'tsc');
  if (!existsSync(tscBin)) fail(`TypeScript がありません: ${tscBin}（root で pnpm install を実行してください）`);
  console.log('[package] client をビルドします（tsc -p .）');
  try {
    execFileSync(process.execPath, [tscBin, '-p', '.'], { cwd: clientDir, stdio: 'inherit' });
  } catch {
    fail('client のビルド（tsc）に失敗しました');
  }
  if (!existsSync(join(clientDir, 'dist', 'main.js'))) {
    fail('client/dist/main.js がありません（tsc の出力を確認してください）');
  }
  // ネイティブ成果物の存在チェック。
  for (const f of [EXE_SRC, ADDON_SRC]) {
    if (!existsSync(f)) {
      fail(
        `${f} がありません。` +
          (f.endsWith('.exe')
            ? 'native のビルド手順（README/AGENTS.md）で exe を用意してください'
            : 'native/transfer-addon/build.ps1 で addon をビルドしてください'),
      );
    }
  }
  let packager;
  try {
    ({ packager } = require(join(repoRoot, 'node_modules', '@electron', 'packager', 'dist', 'index.js')));
  } catch {
    try {
      ({ packager } = require(join(repoRoot, 'node_modules', '@electron', 'packager')));
    } catch {
      fail('@electron/packager がありません（root で pnpm install を実行してください）');
    }
  }
  // 本体に必要なものだけ入れる（src・test・scripts・logs・設定は入れない）。
  const keep = [/^\/package\.json$/, /^\/dist(\/|$)/, /^\/pages(\/|$)/];
  console.log('[package] Electron パッケージを作成します');
  const [appOut] = await packager({
    dir: clientDir,
    out: join(clientDir, 'dist-package'),
    name: 'XRiftClient',
    platform: 'win32',
    arch: 'x64',
    overwrite: true,
    asar: false,
    appVersion: pkg.version,
    extraResource: [EXE_SRC, ADDON_SRC],
    ignore: (p) => p !== '' && !keep.some((r) => r.test(p.replace(/\\/g, '/'))),
  });
  console.log(`[package] 出力: ${appOut}`);
  // テストページ（pages/*.html）の importmap は client 相対で
  // `../../node_modules/three/build/three.module.js` を指す。
  // 配布物では resources/app/pages/... になるため、解決先は
  // resources/node_modules/... である。開発時のみ存在するパスなので、
  // 配布物にも同梱する（相対パスを変えずに済ませる）。
  // three.module.js は ./three.core.js を import するため両方をコピーする
  // （three.core.js 自体は追加の相対 import を持たないことを確認済み）。
  const THREE_FILES = ['three.module.js', 'three.core.js'];
  const threeSrcDir = join(repoRoot, 'node_modules', 'three', 'build');
  for (const f of THREE_FILES) {
    if (!existsSync(join(threeSrcDir, f))) {
      fail(`three がありません: ${join(threeSrcDir, f)}（root で pnpm install を実行してください）`);
    }
  }
  const resources = join(appOut, 'resources');
  const threeDstDir = join(resources, 'node_modules', 'three', 'build');
  mkdirSync(threeDstDir, { recursive: true });
  for (const f of THREE_FILES) {
    copyFileSync(join(threeSrcDir, f), join(threeDstDir, f));
    const kb = (statSync(join(threeDstDir, f)).size / 1024).toFixed(0);
    console.log(`[package] three 同梱: node_modules/three/build/${f} (${kb}KB)`);
  }
  // 生成物の一覧（exe・addon・dist・pages の確認用）。
  const top = readdirSync(appOut);
  console.log(`[package] top: ${top.join(', ')}`);
  if (existsSync(resources)) {
    console.log(`[package] resources: ${readdirSync(resources).join(', ')}`);
  }
  let total = 0;
  const walk = (dir) => {
    for (const e of readdirSync(dir)) {
      const p = join(dir, e);
      const st = statSync(p);
      if (st.isDirectory()) walk(p);
      else total += st.size;
    }
  };
  walk(appOut);
  console.log(`[package] 合計 ${(total / 1024 / 1024).toFixed(1)}MB`);
  console.log(`[package] 起動確認: "${join(appOut, 'XRiftClient.exe')}" --self-test`);
}

main().catch((e) => {
  console.error(`[package] 失敗: ${e instanceof Error ? e.stack ?? e.message : String(e)}`);
  process.exit(1);
});
