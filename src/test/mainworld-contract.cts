// electron/webxr-mainworld.cjs が executeInMainWorld に渡せる自己完結な関数であり、
// 最小のブラウザ風サンドボックスに実際にインストールできることを固定する。
// 外側の変数への参照漏れは vm 実行時の ReferenceError として検出する。
// 実物の MessageChannel は使わない（スタブで置き換え、ハンドルを残さない）。
import fsMod = require('node:fs')
import path = require('node:path')
import vmMod = require('node:vm')

const file = path.join(__dirname, '..', 'electron', 'webxr-mainworld.cjs')
const src = fsMod.readFileSync(file, 'utf8')

const failures: string[] = []
const check = (name: string, ok: boolean): void => {
  if (!ok) failures.push(name)
}

// require で読んだ export が引数なしの関数である（executeInMainWorld は関数を受け取る）。
const loaded: unknown = require(file)
const fn: ((...args: never[]) => unknown) | null =
  typeof loaded === 'function' && (loaded as (...args: never[]) => unknown).length === 0
    ? (loaded as (...args: never[]) => unknown)
    : null
check('module.exports が引数なしの関数である', fn !== null)

// ソース全体に外部参照が無い（自己完結している）。
check('ソース全体で module.exports が1回だけ現れる', (src.match(/module\.exports/g) ?? []).length === 1)
check('トップレベルの import が無い', !/^\s*import(\s|\(|;)/m.test(src))
check('トップレベルの require( が無い', !src.includes('require('))

if (fn !== null) {
  const body = fn.toString()
  // 関数本体が外側を参照していない（本体の文字列で確認する）。
  for (const token of ['require(', 'module.exports', 'exports.', '__dirname', '__filename']) {
    check(`関数本体に ${token} が含まれない`, !body.includes(token))
  }
  // 本体が Function としてパースできる。
  let parsed: unknown = null
  try {
    parsed = new Function(`return (${body})`)() as unknown
  } catch {
    parsed = null
  }
  check('関数本体が Function としてパースできる', typeof parsed === 'function')
  // 最小のブラウザ風サンドボックスに実際にインストールし、自由変数の参照漏れと公開物を検証する。
  try {
    const stubChannel: { port1: Record<string, unknown>; port2: { postMessage: () => void } } = {
      port1: {},
      port2: { postMessage(): void {} },
    }
    // MessageChannel は function 式で渡す。インストール側が new で呼ぶため、アロー関数では構築できず失敗する。
    type InstallSandbox = {
      console: Console
      Event: typeof Event
      EventTarget: typeof EventTarget
      MessageChannel: () => typeof stubChannel
      DOMException: typeof DOMException
      performance: Performance
      setTimeout: typeof setTimeout
      navigator: Record<string, unknown>
      window: Record<string, unknown>
    }
    const sandbox: InstallSandbox = {
      console,
      Event,
      EventTarget,
      MessageChannel: function () {
        return stubChannel
      },
      DOMException,
      performance,
      setTimeout,
      navigator: {},
      window: { __xriftOpenXR: {} },
    }
    vmMod.createContext(sandbox)
    vmMod.runInContext(`(${body})()`, sandbox, { timeout: 5000 })
    check('インストール後に window.__xriftWebXRInstalled が true である', sandbox.window.__xriftWebXRInstalled === true)
    check('インストール後に navigator.xr が object である', typeof sandbox.navigator.xr === 'object')
    check('インストール後に window.XRSession が function である', typeof sandbox.window.XRSession === 'function')
    check('インストール後に window.XRWebGLLayer が function である', typeof sandbox.window.XRWebGLLayer === 'function')
  } catch (err) {
    check(`インストール実行が成功する (${err instanceof Error ? err.message : String(err)})`, false)
  }
} else {
  check('関数本体に require( が含まれない', false)
  check('関数本体に module.exports が含まれない', false)
  check('関数本体に exports. が含まれない', false)
  check('関数本体に __dirname が含まれない', false)
  check('関数本体に __filename が含まれない', false)
  check('関数本体が Function としてパースできる', false)
  check('インストール後に window.__xriftWebXRInstalled が true である', false)
  check('インストール後に navigator.xr が object である', false)
  check('インストール後に window.XRSession が function である', false)
  check('インストール後に window.XRWebGLLayer が function である', false)
}

if (failures.length > 0) {
  for (const f of failures) console.error(`[mainworld-contract] NG: ${f}`)
  process.exit(1)
}
console.log('[mainworld-contract] OK')
