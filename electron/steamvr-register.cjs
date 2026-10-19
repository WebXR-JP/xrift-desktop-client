// SteamVR のライブラリへの登録（アプリ内から行う版。配布版には Node.js が無いため）
//
// .vrmanifest をユーザーデータに書き、SteamVR の openvr_api.dll（IVRApplications）で登録する。
// SteamVR が起動している必要がある。

const { app } = require('electron')
const fs = require('node:fs')
const path = require('node:path')

const APP_KEY = 'net.xrift.openxr-client'

function findOpenVRDll() {
  const vrpath = path.join(process.env.LOCALAPPDATA ?? '', 'openvr', 'openvrpaths.vrpath')
  const candidates = []
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

function manifestPath() {
  return path.join(app.getPath('userData'), 'xrift.vrmanifest')
}

function writeManifest({ world } = {}) {
  // 配布版は exe だけで起動できる。開発時（electron.exe）はアプリのフォルダを引数に渡す
  const args = []
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

function native() {
  return require(path.join(__dirname, '..', 'bin', 'xrift_openxr.node'))
}

function registerSteamVR(opts) {
  const dll = findOpenVRDll()
  const file = writeManifest(opts)
  const r = native().steamvrManifest('add', dll, file)
  if (!r.ok) throw new Error(r.error || '登録に失敗しました')
  return file
}

function unregisterSteamVR() {
  const r = native().steamvrManifest('remove', findOpenVRDll(), manifestPath())
  if (!r.ok) throw new Error(r.error || '解除に失敗しました')
}

module.exports = { registerSteamVR, unregisterSteamVR, APP_KEY }
