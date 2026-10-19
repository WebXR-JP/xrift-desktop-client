// Dawn (npm: webgpu) 上に three.js の WebGPURenderer を立ち上げる
import { create, globals } from 'webgpu'
import { getImagePixels } from '../env/dom.js'

let gpu = null

/** navigator.gpu を Dawn で用意する（WebGPURenderer の生成より前に呼ぶ） */
export function installWebGPU(dawnOptions = []) {
  Object.assign(globalThis, globals)
  gpu = create(dawnOptions)
  patchDrawArgs()
  Object.defineProperty(globalThis.navigator, 'gpu', { value: gpu, configurable: true, writable: true })
  return gpu
}

// ブラウザは draw 系の整数引数を WebIDL の unsigned long 変換（Infinity → 0 など）で丸めるが、
// Dawn の Node バインディングは範囲外で例外を投げる。InstancedBufferGeometry の
// instanceCount（既定 Infinity）などがそのまま来るので、ブラウザと同じ変換をかける
function toUint32(v) {
  if (typeof v !== 'number') return v
  if (!Number.isFinite(v)) return 0
  return Math.trunc(v) >>> 0
}
function patchDrawArgs() {
  for (const cls of [globalThis.GPURenderPassEncoder, globalThis.GPURenderBundleEncoder]) {
    const proto = cls?.prototype
    if (!proto || proto.__xriftPatched) continue
    proto.__xriftPatched = true
    for (const name of ['draw', 'drawIndexed']) {
      const orig = proto[name]
      if (typeof orig !== 'function') continue
      proto[name] = function (...args) {
        return orig.apply(this, args.map(toUint32))
      }
    }
  }
}

/** プロセス終了時に Dawn を解放する（参照が残るとプロセスが終わらない） */
export function releaseWebGPU() {
  try {
    delete globalThis.navigator.gpu
  } catch {}
  gpu = null
}

function premultiply(px) {
  const d = new Uint8Array(px.data)
  for (let i = 0; i < d.length; i += 4) {
    const a = d[i + 3] / 255
    d[i] *= a
    d[i + 1] *= a
    d[i + 2] *= a
  }
  return { width: px.width, height: px.height, data: d }
}

/**
 * Dawn の Node バインディングには copyExternalImageToTexture（DOM 画像の転送）が無いので、
 * デコード済み RGBA を writeTexture で送る経路に差し替える
 */
function patchTextureUpload(renderer) {
  const utils = renderer.backend.textureUtils
  if (!utils || utils.__xriftPatched) return
  utils.__xriftPatched = true
  const warned = new WeakSet()
  utils._copyImageToTexture = function (image, textureGPU, textureDescriptorGPU, originDepth, flipY, premultiplyAlpha, mipLevel = 0) {
    let px = getImagePixels(image)
    if (!px) {
      if (image && typeof image === 'object' && !warned.has(image)) {
        warned.add(image)
        if (!(image instanceof globalThis.HTMLVideoElement)) console.warn('[renderer] unsupported texture image source:', image?.constructor?.name)
      }
      return
    }
    const bpt = this._getBytesPerTexel(textureDescriptorGPU.format)
    if (bpt !== 4) {
      console.warn('[renderer] image upload to non-8bit format is not supported:', textureDescriptorGPU.format)
      return
    }
    if (premultiplyAlpha) px = premultiply(px)
    this._copyBufferToTexture(px, textureGPU, textureDescriptorGPU, originDepth, flipY, 0, mipLevel)
  }
}

/**
 * R3F は gl.xr.setAnimationLoop() を呼ぶが、three の XRManager（WebGPU 系）には無いので補う。
 * XR 中は XRManager の内部ループからユーザーループ（_currentAnimationLoop）が呼ばれる
 */
function patchXRAnimationLoop(renderer) {
  const xr = renderer.xr
  if (typeof xr.setAnimationLoop === 'function') return
  xr.setAnimationLoop = (cb) => {
    xr._currentAnimationLoop = cb
    if (!xr.isPresenting) renderer._animation.setAnimationLoop(cb)
  }
}

/**
 * XR 中は three の出力パス（中間バッファ → 外部テクスチャ）を使わず、リニア HDR のまま
 * XRGPUBinding のテクスチャへ直接描かせる。トーンマップと sRGB 化は XR セッション側
 * （src/xr/output-pass.js）が renderer の設定に合わせて行う
 */
function patchXROutput(renderer, THREE) {
  const get = (name) => {
    for (let p = Object.getPrototypeOf(renderer); p; p = Object.getPrototypeOf(p)) {
      const d = Object.getOwnPropertyDescriptor(p, name)
      if (d?.get) return d.get
    }
    throw new Error(`Renderer.${name} getter not found`)
  }
  const needsFB = get('needsFrameBufferTarget')
  const curTone = get('currentToneMapping')
  const curColor = get('currentColorSpace')
  const inXR = () => renderer.xr.isPresenting && renderer.xr._isWebGPUSession?.()
  Object.defineProperties(renderer, {
    needsFrameBufferTarget: { configurable: true, get() { return inXR() ? false : needsFB.call(this) } },
    currentToneMapping: { configurable: true, get() { return inXR() ? THREE.NoToneMapping : curTone.call(this) } },
    currentColorSpace: { configurable: true, get() { return inXR() ? THREE.ColorManagement.workingColorSpace : curColor.call(this) } },
  })
  renderer.xr.addEventListener('sessionstart', () => {
    const session = renderer.xr.getSession()
    if (session) {
      session._outputProvider = () => ({
        toneMapping: renderer.toneMapping,
        exposure: renderer.toneMappingExposure,
      })
    }
  })
}

export async function createRenderer({ canvas, antialias = true, ...params } = {}) {
  const THREE = await import('three')
  const { WebGPURenderer } = await import('three/webgpu')
  const renderer = new WebGPURenderer({
    canvas,
    antialias,
    powerPreference: 'high-performance',
    ...params,
  })
  await renderer.init()
  patchTextureUpload(renderer)
  patchXRAnimationLoop(renderer)
  patchXROutput(renderer, THREE)
  return renderer
}
