// SteamVR のライブラリに「XRift VR」を登録する
//
//   pnpm register              登録（SteamVR を起動しておくこと）
//   pnpm register -- --remove  登録解除
//   pnpm register -- --world <id>  既定で入るワールドを固定して登録
//
// SteamVR は Electron（専用ブラウザ）を直接起動する。

import { createRequire } from 'node:module'
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { parseArgs } from 'node:util'
import { createCanvas } from '@napi-rs/canvas'

const require = createRequire(import.meta.url)
const root = join(dirname(fileURLToPath(import.meta.url)), '..')
const APP_KEY = 'net.xrift.openxr-client'

const { values } = parseArgs({ options: { remove: { type: 'boolean' }, world: { type: 'string' }, scale: { type: 'string' } } })

function findSteamVR(): string {
  const vrpath = join(process.env.LOCALAPPDATA ?? '', 'openvr', 'openvrpaths.vrpath')
  const candidates: string[] = []
  if (existsSync(vrpath)) {
    try {
      candidates.push(...(JSON.parse(readFileSync(vrpath, 'utf8')).runtime ?? []))
    } catch {}
  }
  candidates.push('C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR')
  for (const dir of candidates) {
    const dll = join(dir, 'bin', 'win64', 'openvr_api.dll')
    if (existsSync(dll)) return dll
  }
  throw new Error('SteamVR が見つかりません（openvrpaths.vrpath / 既定のインストール先）')
}

function makeIcon(file: string): void {
  // SteamVR のライブラリ用サムネイル（460x215 推奨）
  const c = createCanvas(460, 215)
  const ctx = c.getContext('2d')
  const g = ctx.createLinearGradient(0, 0, 460, 215)
  g.addColorStop(0, '#1d1036')
  g.addColorStop(1, '#5b2fb8')
  ctx.fillStyle = g
  ctx.fillRect(0, 0, 460, 215)
  ctx.strokeStyle = 'rgba(255,255,255,0.85)'
  ctx.lineWidth = 6
  ctx.beginPath()
  ctx.arc(95, 107, 44, 0, Math.PI * 2)
  ctx.stroke()
  ctx.beginPath()
  ctx.arc(95, 107, 22, Math.PI * 0.2, Math.PI * 1.7)
  ctx.stroke()
  ctx.fillStyle = '#ffffff'
  ctx.font = 'bold 64px sans-serif'
  ctx.fillText('XRIFT', 165, 125)
  ctx.font = '22px sans-serif'
  ctx.fillStyle = 'rgba(255,255,255,0.75)'
  ctx.fillText('VR  ·  OpenXR / WebGPU', 168, 160)
  writeFileSync(file, c.toBuffer('image/png'))
}

const dll = findSteamVR()
const { steamvrManifest } = require(join(root, 'bin', 'xrift_openxr.node'))
const manifestPath = join(root, 'xrift.vrmanifest')

if (values.remove) {
  const r = steamvrManifest('remove', dll, manifestPath)
  console.log(r.ok ? '登録を解除しました' : `解除に失敗: ${r.error}`)
  process.exit(r.ok ? 0 : 1)
}

const assets = join(root, 'assets')
mkdirSync(assets, { recursive: true })
const icon = join(assets, 'steamvr-library.png')
makeIcon(icon)

const binary = join(root, 'node_modules', 'electron', 'dist', 'electron.exe')
if (!existsSync(binary)) throw new Error(`起動ファイルがありません: ${binary}（pnpm install を実行してください）`)
const args = [`"${root}"`]
if (values.world) args.push('--world', `"${values.world}"`)
if (values.scale) args.push('--scale', values.scale)

const manifest = {
  source: 'builtin',
  applications: [
    {
      app_key: APP_KEY,
      launch_type: 'binary',
      binary_path_windows: binary,
      arguments: args.join(' '),
      working_directory: root,
      image_path: icon,
      is_dashboard_overlay: false,
      strings: {
        en_us: { name: 'XRift VR', description: 'Dedicated XRift browser: app.xrift.net with WebXR bridged to OpenXR' },
        ja_jp: { name: 'XRift VR', description: 'XRift 専用ブラウザ（本家 app.xrift.net の WebXR を OpenXR / SteamVR に接続）' },
      },
    },
  ],
}
writeFileSync(manifestPath, JSON.stringify(manifest, null, 2))

const r = steamvrManifest('add', dll, manifestPath)
if (!r.ok) {
  console.error(`登録に失敗しました: ${r.error}`)
  process.exit(1)
}
const check = steamvrManifest('check', dll, APP_KEY)
console.log(`SteamVR に登録しました（${APP_KEY}${check.installed ? '' : ' / 確認できず'}）`)
console.log(`  manifest: ${manifestPath}`)
console.log('  SteamVR のダッシュボード → ライブラリ（すべて / ソース: 非Steam）に「XRift VR」が出ます')
