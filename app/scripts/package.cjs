// 新アプリ（M5）の配布パッケージを作る: app/dist-package/XRiftClient-win32-x64/
//
//   npm run package            app のビルド + パッケージ
//
// root の node_modules の @electron/packager を使う（app 自体に依存は無い）。
// 同梱物: app/dist、app/pages、app/package.json（本体）、
//   xrift_new.exe + xrift_transfer_addon.node（extraResource → resources/ 直下）。
// main は packagedResource() で resources/ 直下から解決する（開発時は従来パス）。
'use strict';

const { execFileSync } = require('node:child_process');
const { existsSync, readdirSync, statSync } = require('node:fs');
const { join } = require('node:path');

const appDir = join(__dirname, '..');
const repoRoot = join(appDir, '..');
const pkg = require(join(appDir, 'package.json'));

const EXE_SRC = join(repoRoot, 'native_new', 'build', 'windows', 'Release', 'xrift_new.exe');
const ADDON_SRC = join(repoRoot, 'native_new', 'transfer-addon', 'build', 'Release', 'xrift_transfer_addon.node');

function fail(msg) {
  console.error(`[package] 失敗: ${msg}`);
  process.exit(1);
}

async function main() {
  // 事前ビルド（app の tsc）。dist/main.js が無ければ明確に落とす。
  const tscBin = join(repoRoot, 'node_modules', 'typescript', 'bin', 'tsc');
  if (!existsSync(tscBin)) fail(`TypeScript がありません: ${tscBin}（root で pnpm install を実行してください）`);
  console.log('[package] app をビルドします（tsc -p .）');
  try {
    execFileSync(process.execPath, [tscBin, '-p', '.'], { cwd: appDir, stdio: 'inherit' });
  } catch {
    fail('app のビルド（tsc）に失敗しました');
  }
  if (!existsSync(join(appDir, 'dist', 'main.js'))) {
    fail('app/dist/main.js がありません（tsc の出力を確認してください）');
  }
  // ネイティブ成果物の存在チェック。
  for (const f of [EXE_SRC, ADDON_SRC]) {
    if (!existsSync(f)) {
      fail(
        `${f} がありません。` +
          (f.endsWith('.exe')
            ? 'native_new のビルド手順（README/AGENTS.md）で exe を用意してください'
            : 'native_new/transfer-addon/build.ps1 で addon をビルドしてください'),
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
    dir: appDir,
    out: join(appDir, 'dist-package'),
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
  // 生成物の一覧（exe・addon・dist・pages の確認用）。
  const top = readdirSync(appOut);
  console.log(`[package] top: ${top.join(', ')}`);
  const resources = join(appOut, 'resources');
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
