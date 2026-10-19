// XRift のワールド / アイテム（@originjs/vite-plugin-federation の remoteEntry.js）を読み込む
//
// バンドルは react / three / @react-three/* / @xrift/world-components をホストから借りる。
// 照合に使う「版キー」は @xrift/world-components の FEDERATION_SHARED_VERSIONS が正本
// （実際にインストールされている版ではなく、この札で照合される）。

import { FEDERATION_SHARED_VERSIONS } from '@xrift/world-components'

const SHARED_IMPORTS = {
  react: () => import('react'),
  'react-dom': () => import('react-dom'),
  'react/jsx-runtime': () => import('react/jsx-runtime'),
  three: () => import('three'),
  'three/addons/loaders/GLTFLoader.js': () => import('three/examples/jsm/loaders/GLTFLoader.js'),
  'three/addons/loaders/DRACOLoader.js': () => import('three/examples/jsm/loaders/DRACOLoader.js'),
  'three/addons/loaders/KTX2Loader.js': () => import('three/examples/jsm/loaders/KTX2Loader.js'),
  '@react-three/fiber': () => import('@react-three/fiber'),
  '@react-three/rapier': () => import('@react-three/rapier'),
  '@react-three/drei': () => import('@react-three/drei'),
  '@react-three/uikit': () => import('@react-three/uikit'),
  '@pmndrs/uikit': () => import('@pmndrs/uikit'),
  '@xrift/world-components': () => import('@xrift/world-components'),
}

let shareScope = null

/** 共有スコープ。読み込みは遅延（ワールドが要求したものだけ import する） */
export function getShareScope() {
  if (shareScope) return shareScope
  shareScope = {}
  for (const [name, load] of Object.entries(SHARED_IMPORTS)) {
    const version = FEDERATION_SHARED_VERSIONS[name] ?? '0.0.0'
    let cached = null
    shareScope[name] = {
      [version]: {
        get: async () => {
          cached ??= load().catch((err) => {
            console.error(`[federation] shared module "${name}" failed to load:`, err.message)
            throw err
          })
          const mod = await cached
          return () => mod
        },
        loaded: true,
        scope: 'default',
      },
    }
  }
  return shareScope
}

/** remoteEntry.js の URL から、ワールドが相対で読むアセットの基準 URL（末尾 /）を作る */
export function baseUrlFromSceneUrl(sceneUrl) {
  return sceneUrl.slice(0, sceneUrl.lastIndexOf('/') + 1)
}

async function loadExposed(sceneUrl, exposeName, exportName) {
  const container = await import(sceneUrl)
  await container.init(getShareScope())
  const factory = await container.get(exposeName)
  const mod = factory()
  const Component = typeof mod === 'function' ? mod : mod?.[exportName] ?? mod?.default
  if (typeof Component !== 'function' && typeof Component?.$$typeof !== 'symbol') {
    throw new Error(`${exposeName} に ${exportName} コンポーネントがありません`)
  }
  return Component
}

export function loadWorldComponent(sceneUrl) {
  return loadExposed(sceneUrl, './World', 'World')
}

export async function loadItemComponent(sceneUrl) {
  return loadExposed(sceneUrl, './Item', 'Item')
}
