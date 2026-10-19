// XRift 専用ブラウザ（Electron）: 本家フロントエンド app.xrift.net をそのまま表示し、
// ページの WebXR（navigator.xr）を OpenXR / SteamVR に繋ぐ。
//
//   electron .                         本家のトップ
//   electron . --world <ワールドID>     そのワールドを開く
//   electron . --scale 0.8             SteamVR 推奨解像度に対する倍率（既定 1）

import { app, BrowserWindow, Menu, session, shell, desktopCapturer, dialog, crashReporter } from 'electron'
import path = require('node:path')

const APP_ORIGIN = 'https://app.xrift.net'

// ログ（%LOCALAPPDATA%\xrift-openxr-client\browser.log）。ランチャー経由だとコンソールが無いので
import fsMod = require('node:fs')
const logDir = path.join(process.env.LOCALAPPDATA || require('node:os').tmpdir(), 'xrift-openxr-client')
fsMod.mkdirSync(logDir, { recursive: true })
const logFile = path.join(logDir, 'browser.log')
try {
  if (fsMod.existsSync(logFile) && fsMod.statSync(logFile).size > 5e6) fsMod.renameSync(logFile, logFile + '.old')
} catch {}
const logLine = (s: string): void => {
  const line = new Date().toISOString() + ' ' + s
  console.log(line)
  try {
    fsMod.appendFileSync(logFile, line + '\n')
  } catch {}
}
process.on('uncaughtException', (e) => logLine('[main] uncaught ' + (e && e.stack)))
const argv = process.argv.slice(1)
const opt = (name: string): string | undefined => {
  const i = argv.indexOf(`--${name}`)
  if (i >= 0 && argv[i + 1] && !argv[i + 1].startsWith('--')) return argv[i + 1]
  const eq = argv.find((a) => a.startsWith(`--${name}=`))
  return eq ? eq.slice(name.length + 3) : undefined
}
const world = opt('world')
const startUrl = opt('url') ?? (world ? `${APP_ORIGIN}/world/${encodeURIComponent(world)}` : `${APP_ORIGIN}/`)
const scale = opt('scale') ?? '1'
const backend = opt('backend') ?? 'openvr'
const selftest = argv.includes('--selftest')
const forceRender = selftest || argv.includes('--force-render')

// SteamVR 登録は使うときだけ読み込む（起動を速く保つ）。型は emit 先の .cjs を指す。
const steamvr = (): typeof import('./steamvr-register.cjs') => require('./steamvr-register.cjs')

// VR 中はデスクトップのウィンドウが隠れていても描画・タイマーを止めない
app.commandLine.appendSwitch('disable-background-timer-throttling')
app.commandLine.appendSwitch('disable-renderer-backgrounding')
app.commandLine.appendSwitch('disable-backgrounding-occluded-windows')
app.commandLine.appendSwitch('disable-features', 'CalculateNativeWinOcclusion')
app.commandLine.appendSwitch('force_high_performance_gpu')
// デスクトップのモニタの垂直同期（60Hz など）に描画が縛られないようにする（VR は 72〜120Hz）
app.commandLine.appendSwitch('disable-gpu-vsync')
app.commandLine.appendSwitch('ignore-gpu-blocklist')
app.commandLine.appendSwitch('enable-gpu-rasterization')
app.commandLine.appendSwitch('enable-zero-copy')
// 実験用: XRIFT_CHROMIUM_FLAGS="--flag1 --flag2=value"
for (const f of (process.env.XRIFT_CHROMIUM_FLAGS || '').split(/s+/).filter(Boolean)) {
  const [k, ...v] = f.replace(/^--/, '').split('=')
  app.commandLine.appendSwitch(k, v.join('='))
}
if (process.env.XRIFT_NO_VSYNC_OFF) app.commandLine.removeSwitch('disable-gpu-vsync')
if (process.env.XRIFT_WEBGPU_ADAPTER) app.commandLine.appendSwitch('use-webgpu-adapter', process.env.XRIFT_WEBGPU_ADAPTER)
app.commandLine.appendSwitch('autoplay-policy', 'no-user-gesture-required')

app.setName('XRift VR')
// クラッシュダンプを %APPDATA%\XRift VR\Crashpad に残す（送信はしない）
crashReporter.start({ uploadToServer: false })
// 自己テストは別のユーザーデータで動かす（普段使いのウィンドウと衝突しないように）
if (argv.includes('--selftest')) app.setPath('userData', path.join(app.getPath('temp'), 'xrift-vr-selftest'))
else if (!app.requestSingleInstanceLock()) {
  app.quit()
  process.exit(0)
}

const isAppHost = (u: string): boolean => {
  try {
    const h = new URL(u).hostname
    return h === 'xrift.net' || h.endsWith('.xrift.net')
  } catch {
    return false
  }
}
// VRoid Hub などのログイン画面はアプリ内で開く
const isAuthHost = (u: string): boolean => {
  try {
    const h = new URL(u).hostname
    return h.endsWith('vroid.com') || h.endsWith('pixiv.net') || h.endsWith('accounts.google.com')
  } catch {
    return false
  }
}

function buildMenu(win: BrowserWindow): Menu {
  const wc = win.webContents
  return Menu.buildFromTemplate([
    {
      label: 'XRift',
      submenu: [
        { label: 'ホーム', accelerator: 'Alt+Home', click: () => wc.loadURL(`${APP_ORIGIN}/`) },
        { label: '戻る', accelerator: 'Alt+Left', click: () => wc.navigationHistory.canGoBack() && wc.navigationHistory.goBack() },
        { label: '進む', accelerator: 'Alt+Right', click: () => wc.navigationHistory.canGoForward() && wc.navigationHistory.goForward() },
        { label: '再読み込み', accelerator: 'CmdOrCtrl+R', click: () => wc.reload() },
        { type: 'separator' },
        {
          label: 'VR に入る',
          accelerator: 'CmdOrCtrl+Shift+V',
          // 本家の「🥽 VR」ボタンを押すのと同じ（インスタンスに入っている必要がある）
          click: () => wc.executeJavaScript(`(() => { const b = document.querySelector('button[title="VR"]'); if (b) { b.click(); return true } return false })()`, true),
        },
        { label: 'VR を終了', accelerator: 'CmdOrCtrl+Shift+X', click: () => wc.executeJavaScript('navigator.xr && navigator.xr._active && navigator.xr._active.end()') },
        { type: 'separator' },
        {
          label: 'SteamVR のライブラリに登録',
          click: () => {
            try {
              const file = steamvr().registerSteamVR()
              logLine('[xrift] registered to SteamVR: ' + file)
              dialog.showMessageBox(win, { type: 'info', title: 'XRift VR', message: 'SteamVR のライブラリに「XRift VR」を登録しました。' })
            } catch (err) {
              dialog.showMessageBox(win, { type: 'error', title: 'XRift VR', message: '登録できませんでした: ' + (err instanceof Error ? err.message : String(err)) + '（SteamVR を起動してから試してください）' })
            }
          },
        },
        {
          label: 'SteamVR の登録を解除',
          click: () => {
            try {
              steamvr().unregisterSteamVR()
              dialog.showMessageBox(win, { type: 'info', title: 'XRift VR', message: '登録を解除しました。' })
            } catch (err) {
              dialog.showMessageBox(win, { type: 'error', title: 'XRift VR', message: '解除できませんでした: ' + (err instanceof Error ? err.message : String(err)) })
            }
          },
        },
        { type: 'separator' },
        { role: 'togglefullscreen', label: 'フルスクリーン' },
        { role: 'toggleDevTools', label: '開発者ツール', accelerator: 'F12' },
        { type: 'separator' },
        { role: 'quit', label: '終了' },
      ],
    },
    { role: 'editMenu', label: '編集' },
  ])
}

async function createWindow(): Promise<BrowserWindow> {
  const ses = session.fromPartition('persist:xrift')
  // Electron の名前を含まない、普通の Chrome と同じ UA にする（ログイン画面などのため）
  const ua = ses.getUserAgent().replace(/\s(Electron|xrift-openxr-client|XRift VR)\/\S+/g, '')
  ses.setUserAgent(ua)

  const allowed = new Set(['media', 'microphone', 'audioCapture', 'fullscreen', 'pointerLock', 'clipboard-read', 'clipboard-sanitized-write', 'display-capture', 'notifications', 'speaker-selection'])
  ses.setPermissionRequestHandler((wc, permission, cb, details) => cb(isAppHost(details.requestingUrl ?? wc.getURL()) && allowed.has(permission)))
  ses.setPermissionCheckHandler((wc, permission, origin) => isAppHost(origin) && allowed.has(permission))
  // 画面共有: 画面を選ばせる
  ses.setDisplayMediaRequestHandler(async (_req, cb) => {
    const sources = await desktopCapturer.getSources({ types: ['screen', 'window'] })
    if (!sources.length) return cb({})
    const choice = await dialog.showMessageBox({
      type: 'question',
      title: '画面共有',
      message: '共有する画面 / ウィンドウを選んでください',
      buttons: [...sources.slice(0, 8).map((s) => s.name), 'キャンセル'],
      cancelId: Math.min(sources.length, 8),
    })
    const src = sources[choice.response]
    cb(src ? { video: src, audio: 'loopback' } : {})
  })

  const win = new BrowserWindow({
    width: 1400,
    height: 860,
    title: 'XRift VR',
    icon: path.join(__dirname, '..', 'assets', 'icon.png'),
    backgroundColor: '#14101f',
    webPreferences: {
      partition: 'persist:xrift',
      preload: path.join(__dirname, 'preload.cjs'),
      contextIsolation: true,
      nodeIntegration: false,
      // preload でネイティブアドオンを読むため（ページ側には Node を出さない）
      sandbox: false,
      backgroundThrottling: false,
      additionalArguments: [`--xrift-scale=${scale}`, `--xrift-force-render=${forceRender ? 1 : 0}`, `--xrift-backend=${backend}`],
    },
  })
  Menu.setApplicationMenu(buildMenu(win))

  win.webContents.setWindowOpenHandler(({ url }) => {
    if (isAppHost(url) || isAuthHost(url)) return { action: 'allow', overrideBrowserWindowOptions: { autoHideMenuBar: true, webPreferences: { partition: 'persist:xrift' } } }
    shell.openExternal(url)
    return { action: 'deny' }
  })
  win.webContents.on('will-navigate', (e, url) => {
    if (!isAppHost(url) && !isAuthHost(url)) {
      e.preventDefault()
      shell.openExternal(url)
    }
  })
  // console-message の引数形は Electron の版で変わるため、実行時と同じ単一引数の受け方に絞って型を付ける。
  // ADR: Electron 44 の型は (event, details) だが、実行時の挙動（先頭引数から message/level を読む）を保つため
  // unknown 受け＋最小限の構造キャストに留める。ログ条件式自体は変更しない。
  win.webContents.on('console-message', (e: unknown) => {
    const m = e as { message?: unknown; level?: unknown }
    if (typeof m.message === 'string' && (m.message.includes('[XRift VR]') || m.level === 'error')) logLine(m.message.replace(/%c|color:#a07cff/g, '').trim().slice(0, 2000))
  })
  win.webContents.on('render-process-gone', (_e, d) => logLine(`[xrift] renderer gone: ${d.reason} exitCode=${d.exitCode} (0x${(d.exitCode >>> 0).toString(16)})`))
  app.on('child-process-gone', (_e, d) => logLine(`[xrift] child process gone: ${d.type} ${d.reason} exitCode=${d.exitCode} (0x${(d.exitCode >>> 0).toString(16)})`))
  logLine(`[xrift] start url=${startUrl} backend=${backend} scale=${scale}`)
  // VR の 1 フレームの持ち時間（72Hz なら 13.9ms）に収まるよう、ページと GPU のプロセスを優先度「高」にする
  const os = require('node:os')
  const boost = (): void => {
    const pids = new Set([process.pid, win.webContents.getOSProcessId()])
    for (const p of app.getAppMetrics()) if (p.type === 'GPU' || p.type === 'Tab') pids.add(p.pid)
    for (const pid of pids) {
      try {
        if (pid) os.setPriority(pid, os.constants.priority.PRIORITY_HIGH)
      } catch (err) {
        logLine('[xrift] priority boost failed for ' + pid + ': ' + (err instanceof Error ? err.message : String(err)))
      }
    }
  }
  win.webContents.on('did-finish-load', boost)
  app.on('child-process-gone', () => setTimeout(boost, 2000))
  setTimeout(boost, 3000)

  if (selftest) {
    // 自己テスト: ローカルの three r185 ページで VR セッションを開始し、デスクトップのミラーを保存して終了
    const page = (opt('selftest-page') ?? 'selftest.html').split('?')
    await win.loadFile(path.join(__dirname, page[0]), page[1] ? { search: page[1] } : undefined)
    setTimeout(async () => {
      const img = await win.webContents.capturePage()
      const out = app.isPackaged ? path.join(app.getPath('temp'), 'xrift-browser-selftest.png') : path.join(__dirname, '..', 'test', 'browser-selftest.png')
      require('node:fs').writeFileSync(out, img.toPNG())
      console.log('[xrift] selftest capture saved')
      await win.webContents.executeJavaScript('navigator.xr._active && navigator.xr._active.end()')
      setTimeout(() => app.quit(), 1500)
    }, Number(opt('selftest-seconds') ?? 12) * 1000)
    return win
  }
  await win.loadURL(startUrl)
  // 診断用: --capture <png> [--capture-wait 秒] [--eval <js>]
  const cap = opt('capture')
  if (cap) {
    setTimeout(async () => {
      const js = opt('eval')
      if (js) console.log('[xrift] eval:', JSON.stringify(await win.webContents.executeJavaScript(js).catch((e) => String(e))))
      const img = await win.webContents.capturePage()
      require('node:fs').writeFileSync(cap, img.toPNG())
      console.log('[xrift] captured', cap, win.webContents.getURL())
      if (argv.includes('--capture-quit')) app.quit()
    }, Number(opt('capture-wait') ?? 10) * 1000)
  }
  return win
}

app.on('second-instance', () => {
  const w = BrowserWindow.getAllWindows()[0]
  if (w) {
    if (w.isMinimized()) w.restore()
    w.focus()
  }
})
app.whenReady().then(() => {
  // XRiftVR.exe --register / --unregister: ウィンドウを出さずに SteamVR へ登録して終了
  if (argv.includes('--register') || argv.includes('--unregister')) {
    try {
      const reg = steamvr()
      if (argv.includes('--unregister')) reg.unregisterSteamVR()
      else logLine('[xrift] registered to SteamVR: ' + reg.registerSteamVR({ world }))
      app.exit(0)
    } catch (err) {
      logLine('[xrift] register failed: ' + (err instanceof Error ? err.message : String(err)))
      dialog.showErrorBox('XRift VR', '登録できませんでした: ' + (err instanceof Error ? err.message : String(err)))
      app.exit(1)
    }
    return
  }
  createWindow()
})
app.on('window-all-closed', () => app.quit())
