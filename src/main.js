#!/usr/bin/env node
// XRift VR クライアント（Node.js + WebGPU/Dawn + three.js XRManager → OpenXR / SteamVR）
//
//   node src/main.js                       既定のワールドに VR で入る
//   node src/main.js --world <id|名前|URL>  ワールドを指定
//   node src/main.js --list                公開ワールドの一覧
//   node src/main.js --screenshot out.png  HMD なしでデスクトップ描画を PNG に保存（動作確認用）

import './env/dom.js'
import { parseArgs } from 'node:util'
import { installModuleHooks, setNoCache } from './env/module-hooks.js'

const HELP = `XRift VR client (OpenXR / SteamVR)

使い方: XRiftVR.exe [options]   または   node src/main.js [options]

  -w, --world <id|名前|URL>  入るワールド（ID / xrift.net の URL / 名前の一部 / remoteEntry.js の URL）
  -l, --list                公開ワールドの一覧を表示
      --scale <n>           SteamVR の推奨解像度に対する倍率（既定 1 = SteamVR の設定どおり）
      --backend <name>      openvr（既定。SteamVR ネイティブ）/ openxr
      --no-msaa             MSAA を無効にする（軽くなる）
      --screenshot <file>   VR を起動せず、デスクトップ描画を PNG に保存
      --wait <sec>          --screenshot までの待ち時間（既定 8）
      --no-cache            ワールドのバンドルを再ダウンロードする
  -v, --verbose             フレームレートなどを表示
  -h, --help
`

const { values } = parseArgs({
  options: {
    world: { type: 'string', short: 'w' },
    list: { type: 'boolean', short: 'l' },
    scale: { type: 'string' },
    backend: { type: 'string' },
    'no-msaa': { type: 'boolean' },
    screenshot: { type: 'string' },
    wait: { type: 'string' },
    width: { type: 'string' },
    height: { type: 'string' },
    'no-cache': { type: 'boolean' },
    'force-render': { type: 'boolean' },
    verbose: { type: 'boolean', short: 'v' },
    help: { type: 'boolean', short: 'h' },
  },
  allowPositionals: true,
})

if (values.help) {
  console.log(HELP)
  process.exit(0)
}

// ワールドのコードの非同期エラーで VR ごと落ちないようにする（ログだけ残す）
process.on('uncaughtException', (err) => console.error('[xrift] uncaught:', err?.stack ?? err))
process.on('unhandledRejection', (err) => console.error('[xrift] unhandled rejection:', err?.stack ?? err))

installModuleHooks()
setNoCache(!!values['no-cache'])

if (values.list) {
  const { listWorlds } = await import('./xrift/api.js')
  const worlds = await listWorlds()
  for (const w of worlds) console.log(`${w.id}  ${w.name}${w.owner ? `  (${w.owner.displayName})` : ''}`)
  process.exit(0)
}

const { run } = await import('./host.js')
try {
  await run({
    world: values.world ?? process.env.XRIFT_WORLD,
    scale: values.scale ? Number(values.scale) : 1,
    backend: values.backend ?? 'openvr',
    msaa: !values['no-msaa'],
    screenshot: values.screenshot,
    wait: values.wait ? Number(values.wait) : 8,
    width: values.width ? Number(values.width) : 1280,
    height: values.height ? Number(values.height) : 720,
    verbose: !!values.verbose,
    forceRender: !!values['force-render'],
  })
} catch (err) {
  console.error('\n[xrift] 起動に失敗しました:', err?.stack ?? err)
  if (process.stdin.isTTY && process.env.XRIFT_LAUNCHER) {
    console.error('\nEnter キーで閉じます')
    process.stdin.once('data', () => process.exit(1))
  } else {
    process.exit(1)
  }
}
