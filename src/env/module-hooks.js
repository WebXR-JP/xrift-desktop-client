// Node のモジュール解決フック
//
// 1. https:// の ESM（XRift ワールドの Module Federation バンドル）をそのまま import できるようにする
// 2. リモートモジュール内の裸の指定子（例: 'three/addons/objects/MarchingCubes.js'）を
//    このクライアントの node_modules から解決する
// 3. 拡張子なしの相対 import（tsc 出力の ESM、@xrift/world-components など）を補完する

import { registerHooks } from 'node:module'
import { pathToFileURL } from 'node:url'
import { join } from 'node:path'
import { readFileSync } from 'node:fs'
import { syncFetchToFile } from './sync-fetch.js'

const HOST_PARENT = pathToFileURL(join(process.cwd(), 'package.json')).href
const PROJECT_PARENT = new URL('../../package.json', import.meta.url).href

const isRemote = (u) => typeof u === 'string' && (u.startsWith('https://') || u.startsWith('http://'))
const isRelative = (s) => s.startsWith('./') || s.startsWith('../') || s.startsWith('/')

function mapBare(specifier) {
  if (specifier.startsWith('three/addons/')) return 'three/examples/jsm/' + specifier.slice('three/addons/'.length)
  return specifier
}

// "exports" を持たず "module"（ESM ビルド）を持つパッケージは、バンドラと同じく ESM 側を使う。
// そうしないと @react-three/fiber などが CJS 版になり、three も CJS 版（別インスタンス）を掴んでしまう
const moduleFieldCache = new Map()
function packageNameOf(specifier) {
  if (specifier.startsWith('node:') || specifier.startsWith('#')) return null
  const parts = specifier.split('/')
  const name = specifier.startsWith('@') ? parts.slice(0, 2).join('/') : parts[0]
  return name === specifier ? name : null // サブパス指定は対象外
}
function preferModuleField(specifier, result) {
  const name = packageNameOf(specifier)
  if (!name || !result?.url?.startsWith('file:')) return result
  const marker = '/node_modules/' + name + '/'
  const idx = result.url.lastIndexOf(marker)
  if (idx < 0) return result
  const pkgDirUrl = result.url.slice(0, idx + marker.length)
  let esmUrl = moduleFieldCache.get(pkgDirUrl)
  if (esmUrl === undefined) {
    esmUrl = null
    try {
      const pkg = JSON.parse(readFileSync(new URL('package.json', pkgDirUrl), 'utf8'))
      if (!pkg.exports && typeof pkg.module === 'string') esmUrl = new URL(pkg.module, pkgDirUrl).href
    } catch {}
    moduleFieldCache.set(pkgDirUrl, esmUrl)
  }
  return esmUrl ? { ...result, url: esmUrl, format: 'module' } : result
}

let noCache = false
export function setNoCache(v) {
  noCache = v
}

export function installModuleHooks() {
  registerHooks({
    resolve(specifier, context, nextResolve) {
      if (isRemote(specifier)) return { url: specifier, format: 'module', shortCircuit: true }

      const parent = context.parentURL
      if (isRemote(parent)) {
        if (isRelative(specifier)) {
          return { url: new URL(specifier, parent).href, format: 'module', shortCircuit: true }
        }
        // リモートからの裸の指定子はホスト（このクライアント）の依存で解決する
        const s = mapBare(specifier)
        return preferModuleField(s, nextResolve(s, { ...context, parentURL: PROJECT_PARENT ?? HOST_PARENT }))
      }

      try {
        const s = mapBare(specifier)
        return preferModuleField(s, nextResolve(s, context))
      } catch (err) {
        if (isRelative(specifier) && (err.code === 'ERR_MODULE_NOT_FOUND' || err.code === 'ERR_UNSUPPORTED_DIR_IMPORT')) {
          for (const suffix of ['.js', '/index.js', '.mjs']) {
            try {
              return nextResolve(specifier + suffix, context)
            } catch {}
          }
        }
        throw err
      }
    },

    load(url, context, nextLoad) {
      if (isRemote(url)) {
        const file = syncFetchToFile(url, { noCache })
        return { format: 'module', source: readFileSync(file, 'utf8'), shortCircuit: true }
      }
      return nextLoad(url, context)
    },
  })
}
