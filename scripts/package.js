// 配布用パッケージを作る: dist/XRift-Client-win-x64.zip
//
//   npm run build:native && npm run package
//
// 専用ブラウザ（Electron）だけを同梱する。main / preload は Node の組み込みと electron しか使わないので、
// npm の依存パッケージは入れない（Node.js クライアントはソースから使う）。

import { packager } from '@electron/packager'
import { flipFuses, FuseVersion, FuseV1Options } from '@electron/fuses'
import { createCanvas, loadImage } from '@napi-rs/canvas'
import { execFileSync } from 'node:child_process'
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync, copyFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const root = join(dirname(fileURLToPath(import.meta.url)), '..')
const pkg = JSON.parse(readFileSync(join(root, 'package.json'), 'utf8'))
const dist = join(root, 'dist')

for (const f of ['bin/xrift_openxr.node', 'bin/openxr_loader.dll', 'bin/openvr_api.dll']) {
  if (!existsSync(join(root, f))) throw new Error(`${f} がありません。先に npm run build:native を実行してください`)
}

// exe のアイコン（PNG を埋め込んだ ICO）
async function makeIco(png, out) {
  const img = await loadImage(readFileSync(png))
  const sizes = [256, 64, 48, 32, 16]
  const pngs = sizes.map((s) => {
    const c = createCanvas(s, s)
    c.getContext('2d').drawImage(img, 0, 0, s, s)
    return c.toBuffer('image/png')
  })
  const header = Buffer.alloc(6 + 16 * sizes.length)
  header.writeUInt16LE(0, 0)
  header.writeUInt16LE(1, 2)
  header.writeUInt16LE(sizes.length, 4)
  let offset = header.length
  sizes.forEach((s, i) => {
    const e = 6 + i * 16
    header.writeUInt8(s >= 256 ? 0 : s, e)
    header.writeUInt8(s >= 256 ? 0 : s, e + 1)
    header.writeUInt16LE(1, e + 4)
    header.writeUInt16LE(32, e + 6)
    header.writeUInt32LE(pngs[i].length, e + 8)
    header.writeUInt32LE(offset, e + 12)
    offset += pngs[i].length
  })
  writeFileSync(out, Buffer.concat([header, ...pngs]))
}

mkdirSync(dist, { recursive: true })
const ico = join(dist, 'icon.ico')
await makeIco(join(root, 'assets', 'icon.png'), ico)

const keep = [/^\/package\.json$/, /^\/electron(\/|$)/, /^\/bin(\/|$)/, /^\/assets(\/|$)/, /^\/LICENSE$/, /^\/README\.md$/]
const [appDir] = await packager({
  dir: root,
  out: dist,
  name: 'XRiftVR',
  executableName: 'XRiftVR',
  platform: 'win32',
  arch: 'x64',
  overwrite: true,
  asar: false, // ネイティブアドオン（.node / .dll）を読むため
  prune: false,
  icon: ico,
  appVersion: pkg.version,
  appCopyright: 'KinuX2 (unofficial, not affiliated with XRift)',
  win32metadata: { CompanyName: 'KinuX2', ProductName: 'XRift VR (unofficial test)', FileDescription: 'XRift VR (unofficial test client)' },
  // 必要なものだけ入れる（node_modules・ソース・テスト・ログ・ローカル設定は入れない）
  ignore: (p) => p !== '' && !keep.some((r) => r.test(p.replace(/\\/g, '/'))),
})

// 配布版では「Node として起動」などの抜け道を閉じる
const exe = join(appDir, 'XRiftVR.exe')
await flipFuses(exe, {
  version: FuseVersion.V1,
  [FuseV1Options.RunAsNode]: false,
  [FuseV1Options.EnableNodeOptionsEnvironmentVariable]: false,
  [FuseV1Options.EnableNodeCliInspectArguments]: false,
})

// 解凍してすぐ読めるように
copyFileSync(join(root, 'LICENSE'), join(appDir, 'LICENSE.txt'))
copyFileSync(join(root, 'THIRD_PARTY_NOTICES.md'), join(appDir, 'THIRD_PARTY_NOTICES.md'))
writeFileSync(
  join(appDir, 'はじめにお読みください.txt'),
  [
    'XRift クライアント（非公式テスト） v' + pkg.version,
    '',
    '1. SteamVR を起動しておく',
    '2. XRiftVR.exe を起動し、メニュー「XRift」→「SteamVR のライブラリに登録」（初回のみ）',
    '3. 本家にログインしてインスタンスに入り、「🥽 VR」ボタン（または Ctrl+Shift+V）で VR に入る',
    '',
    'XRift 公式とは関係のない非公式のテスト版です。',
    'https://github.com/KinuX2/XRift-Client',
    '',
  ].join('\r\n'),
)

const zip = join(dist, `XRift-Client-win-x64.zip`)
rmSync(zip, { force: true })
execFileSync('powershell', ['-NoProfile', '-Command', `Compress-Archive -Path '${appDir}\\*' -DestinationPath '${zip}' -CompressionLevel Optimal`], { stdio: 'inherit' })
console.log(`\n[package] ${appDir}\n[package] ${zip}`)
