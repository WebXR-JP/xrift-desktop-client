// electron/webxr-mainworld.cjs が executeInMainWorld に渡せる自己完結な関数であることを固定する。
// ページ内に注入する関数は外側の変数・import を参照できないため、
// このテストはファイルの形（export の形・外部参照の有無・パース可能性）だけを検証する。関数は実行しない。
import fsMod = require('node:fs')
import path = require('node:path')

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

// 関数本体が外側を参照していない（パースした本体の文字列で確認する）。
if (fn !== null) {
  const body = fn.toString()
  for (const token of ['require(', 'module.exports', 'exports.', '__dirname', '__filename']) {
    check(`関数本体に ${token} が含まれない`, !body.includes(token))
  }
  // 本体が Function としてパースできる（実行はしない）。
  let parsed: unknown = null
  try {
    parsed = new Function(`return (${body})`)() as unknown
  } catch {
    parsed = null
  }
  check('関数本体が Function としてパースできる', typeof parsed === 'function')
} else {
  check('関数本体に require( が含まれない', false)
  check('関数本体に module.exports が含まれない', false)
  check('関数本体に exports. が含まれない', false)
  check('関数本体に __dirname が含まれない', false)
  check('関数本体に __filename が含まれない', false)
  check('関数本体が Function としてパースできる', false)
}

if (failures.length > 0) {
  for (const f of failures) console.error(`[mainworld-contract] NG: ${f}`)
  process.exit(1)
}
console.log('[mainworld-contract] OK')
