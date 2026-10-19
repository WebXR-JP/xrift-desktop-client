// クライアント本体: ワールド解決 → WebGPU レンダラ → R3F ルート → OpenXR セッション
import { createElement as h } from 'react'
import { setLocation } from './env/dom.js'
import { installWebGPU, createRenderer, releaseWebGPU } from './render/renderer.js'
import { installWebXR } from './xr/webxr-openxr.js'
import { resolveWorld } from './xrift/api.js'
import { baseUrlFromSceneUrl, loadWorldComponent } from './xrift/federation.js'

// XRIFT_DUMP_XR=<file.png> で、VR に送った両眼の画像を 1 枚保存する（デバッグ用）
function dumpOnce(file) {
  let n = 0
  return (r) => {
    if (++n !== 300) return
    const raw = new Uint8Array(r.data)
    const out = document.createElement('canvas')
    out.width = r.width * 2
    out.height = r.height
    const ctx = out.getContext('2d')
    const id = ctx.createImageData(out.width, out.height)
    const bgra = r.format.startsWith('bgra')
    for (let eye = 0; eye < 2; eye++)
      for (let y = 0; y < r.height; y++)
        for (let x = 0; x < r.width; x++) {
          const s = eye * r.layerBytes + y * r.bytesPerRow + x * 4
          const d = (y * out.width + eye * r.width + x) * 4
          id.data[d] = raw[s + (bgra ? 2 : 0)]
          id.data[d + 1] = raw[s + 1]
          id.data[d + 2] = raw[s + (bgra ? 0 : 2)]
          id.data[d + 3] = 255
        }
    ctx.putImageData(id, 0, 0)
    import('node:fs').then(({ writeFileSync }) => {
      writeFileSync(file, Buffer.from(out.toDataURL('image/png').split(',')[1], 'base64'))
      console.log('[debug] dumped XR frame to', file)
    })
  }
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

export async function run(opts) {
  installWebGPU(opts.dawn ?? [])
  installWebXR({
    appName: 'XRift VR (OpenXR)',
    resolutionScale: opts.scale,
    backend: opts.backend,
    verbose: opts.verbose,
    forceRender: opts.forceRender,
    onReadback: process.env.XRIFT_DUMP_XR ? dumpOnce(process.env.XRIFT_DUMP_XR) : undefined,
  })

  console.log('[xrift] resolving world...')
  const world = await resolveWorld(opts.world)
  const baseUrl = baseUrlFromSceneUrl(world.sceneUrl)
  setLocation(baseUrl)
  console.log(`[xrift] world: ${world.name}${world.owner ? ` by ${world.owner.displayName}` : ''}`)
  console.log(`[xrift] bundle: ${world.sceneUrl}`)
  const World = await loadWorldComponent(world.sceneUrl)

  const THREE = await import('three')
  const { createRoot, extend } = await import('@react-three/fiber')
  // R3F は <Canvas> の中でしか THREE を登録しないので、createRoot を直接使う場合は自分で行う
  extend(THREE)
  // drei の <Text>（troika）は Worker 内で importScripts を使うので、メインスレッドで SDF を作らせる
  try {
    const { configureTextBuilder } = await import('troika-three-text')
    configureTextBuilder({ useWorker: false })
  } catch {}
  const { App } = await import('./app/App.js')

  const width = opts.width
  const height = opts.height
  const canvas = document.createElement('canvas')
  canvas.width = width
  canvas.height = height
  const outputBufferType = world.outputBufferType ? THREE[world.outputBufferType] : undefined
  const renderer = await createRenderer({ canvas, antialias: opts.msaa, ...(outputBufferType ? { outputBufferType } : {}) })

  const camCfg = world.camera ?? {}
  const root = createRoot(canvas)
  await root.configure({
    gl: renderer,
    size: { width, height, top: 0, left: 0 },
    dpr: 1,
    shadows: { type: THREE.PCFShadowMap },
    camera: { fov: camCfg.fov ?? 70, near: camCfg.near ?? 0.05, far: camCfg.far ?? 1000, position: [0, 1.44, 0] },
    frameloop: 'always',
  })
  const movementRef = {
    current: {
      position: { x: 0, y: 0, z: 0 },
      direction: { x: 0, z: 0 },
      horizontalSpeed: 0,
      verticalSpeed: 0,
      rotation: { yaw: 0, pitch: 0 },
      isGrounded: true,
      isJumping: false,
    },
  }
  const stateRef = { current: null }
  root.render(h(App, { World, world, baseUrl, movementRef, teleportRef: { current: null }, rigRef: { current: null }, stateRef }))

  const shutdown = async (code = 0) => {
    try {
      root.unmount()
    } catch {}
    try {
      renderer.dispose()
    } catch {}
    releaseWebGPU()
    process.exit(code)
  }

  if (opts.screenshot) {
    // デスクトップ（HMD なし）での描画確認
    console.log(`[xrift] rendering for ${opts.wait}s, then saving ${opts.screenshot}`)
    await sleep(opts.wait * 1000)
    if (opts.verbose) {
      const st = stateRef.current()
      let meshes = 0
      st.scene.traverse((o) => { if (o.isMesh) meshes++ })
      const p = st.camera.getWorldPosition(new THREE.Vector3())
      console.log('[debug] meshes', meshes, 'camera', p.toArray().map((v) => +v.toFixed(2)), 'renders', renderer.info.render.frameCalls ?? renderer.info.render.calls, 'size', st.size)
    }
    const { captureCanvasPNG } = await import('./render/capture.js')
    await captureCanvasPNG(renderer, canvas, opts.screenshot)
    console.log('[xrift] saved', opts.screenshot)
    return shutdown(0)
  }

  // ---- VR
  const session = await navigator.xr.requestSession('immersive-vr', {
    requiredFeatures: ['webgpu'],
    optionalFeatures: ['local-floor', 'bounded-floor'],
  })
  renderer.xr.setReferenceSpaceType('local-floor')
  await renderer.xr.setSession(session)
  console.log('[xrift] VR session started. Ctrl+C か SteamVR のダッシュボードから終了できます')

  let ending = false
  const end = async () => {
    if (ending) return
    ending = true
    console.log('[xrift] ending session...')
    await Promise.race([session.end(), sleep(3000)])
  }
  process.on('SIGINT', end)
  process.on('SIGTERM', end)
  session.addEventListener('end', () => {
    console.log('[xrift] session ended')
    setTimeout(() => shutdown(0), 100)
  })
}
