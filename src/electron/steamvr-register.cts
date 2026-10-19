// SteamVR のライブラリへの登録（アプリ内から行う版。配布版には Node.js が無いため）
//
// .vrmanifest をユーザーデータに書き、SteamVR の openvr_api.dll（IVRApplications）で登録する。
// SteamVR が起動している必要がある。

import { app } from 'electron'
import fs = require('node:fs')
import path = require('node:path')

const APP_KEY = 'net.xrift.openxr-client'

function findOpenVRDll(): string {
  const vrpath = path.join(process.env.LOCALAPPDATA ?? '', 'openvr', 'openvrpaths.vrpath')
  const candidates: string[] = []
  try {
    candidates.push(...(JSON.parse(fs.readFileSync(vrpath, 'utf8')).runtime ?? []))
  } catch {}
  candidates.push('C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR')
  for (const dir of candidates) {
    const dll = path.join(dir, 'bin', 'win64', 'openvr_api.dll')
    if (fs.existsSync(dll)) return dll
  }
  throw new Error('SteamVR が見つかりません')
}

function manifestPath(): string {
  return path.join(app.getPath('userData'), 'xrift.vrmanifest')
}

function writeManifest(opts: { world?: string } = {}): string {
  const { world } = opts
  // 配布版は exe だけで起動できる。開発時（electron.exe）はアプリのフォルダを引数に渡す
  const args: string[] = []
  if (!app.isPackaged) args.push(`"${app.getAppPath()}"`)
  if (world) args.push('--world', `"${world}"`)
  const icon = path.join(__dirname, '..', 'assets', 'icon.png')
  const manifest = {
    source: 'builtin',
    applications: [
      {
        app_key: APP_KEY,
        launch_type: 'binary',
        binary_path_windows: process.execPath,
        arguments: args.join(' '),
        working_directory: path.dirname(process.execPath),
        image_path: icon,
        is_dashboard_overlay: false,
        strings: {
          en_us: { name: 'XRift VR', description: 'XRift client (unofficial test): app.xrift.net on SteamVR' },
          ja_jp: { name: 'XRift VR', description: 'XRift クライアント（非公式テスト）: 本家 app.xrift.net を SteamVR で' },
        },
      },
    ],
  }
  fs.mkdirSync(path.dirname(manifestPath()), { recursive: true })
  fs.writeFileSync(manifestPath(), JSON.stringify(manifest, null, 2))
  return manifestPath()
}

function native(): any {
  // ネイティブアドオンに型定義は無いため any 境界とする（呼び出し側も any として扱う）。
  return require(path.join(__dirname, '..', 'bin', 'xrift_openxr.node'))
}

function registerSteamVR(opts: { world?: string } = {}): string {
  const dll = findOpenVRDll()
  const file = writeManifest(opts)
  const r = native().steamvrManifest('add', dll, file)
  if (!r.ok) throw new Error(r.error || '登録に失敗しました')
  return file
}

function unregisterSteamVR(): void {
  const r = native().steamvrManifest('remove', findOpenVRDll(), manifestPath())
  if (!r.ok) throw new Error(r.error || '解除に失敗しました')
}

export = { registerSteamVR, unregisterSteamVR, APP_KEY }
