// WebGPU で描けないマテリアルの代替
//
// three の WebGPURenderer は GLSL の ShaderMaterial / RawShaderMaterial を描けない（黒や欠けになる）。
// ワールドのシーンを定期的に見回し、そうしたマテリアルを「ユニフォームから推定した色・テクスチャ」を
// 使う標準マテリアルに差し替える。見た目は完全には一致しないが、空間の形はわかるようになる。

import { useFrame, useThree } from '@react-three/fiber'
import * as THREE from 'three'
import { installTroikaWebGPU } from '../render/troika-webgpu.js'

const BASE_HINT = /(base|albedo|diffuse|ground|floor)/i
const COLOR_HINT = /(color|colour|base|albedo|diffuse|tint|col)$/i
const fallbackCache = new WeakMap()

function pickColor(uniforms) {
  for (const [name, u] of Object.entries(uniforms ?? {})) {
    if (BASE_HINT.test(name) && u?.value?.isColor) return u.value
  }
  let best = null
  for (const [name, u] of Object.entries(uniforms ?? {})) {
    const v = u?.value
    if (v?.isColor) {
      if (COLOR_HINT.test(name)) return v
      best ??= v
    } else if (v?.isVector3 && COLOR_HINT.test(name)) {
      best ??= new THREE.Color(v.x, v.y, v.z)
    }
  }
  return best
}

function pickMap(uniforms) {
  for (const [name, u] of Object.entries(uniforms ?? {})) {
    const v = u?.value
    if (v?.isTexture && !v.isDepthTexture && !/(noise|normal|depth|mask|env)/i.test(name)) return v
  }
  return null
}

function fallbackFor(material) {
  let fb = fallbackCache.get(material)
  if (fb) return fb
  const color = pickColor(material.uniforms)
  const map = pickMap(material.uniforms)
  fb = new THREE.MeshStandardMaterial({
    color: color ? color.clone() : new THREE.Color(0x808080),
    map,
    roughness: 0.8,
    metalness: 0,
    transparent: material.transparent,
    opacity: material.uniforms?.opacity?.value ?? material.opacity ?? 1,
    side: material.side,
    depthWrite: material.depthWrite,
    depthTest: material.depthTest,
    blending: material.blending === THREE.CustomBlending ? THREE.NormalBlending : material.blending,
    wireframe: material.wireframe,
  })
  fb.name = `${material.name || material.type} (WebGPU fallback)`
  fb.userData.originalMaterial = material
  fallbackCache.set(material, fb)
  return fb
}

const needsFallback = (m) => m && !m.isNodeMaterial && (m.isShaderMaterial || m.isRawShaderMaterial)

export function MaterialCompat({ interval = 15 }) {
  const scene = useThree((s) => s.scene)
  const reported = new WeakSet()
  let frame = 0
  useFrame(() => {
    if (frame++ % interval !== 0) return
    scene.traverse((o) => {
      if (o.isTroikaText) {
        installTroikaWebGPU(o)
        return
      }
      if (!o.material) return
      if (Array.isArray(o.material)) {
        if (o.material.some(needsFallback)) o.material = o.material.map((m) => (needsFallback(m) ? fallbackFor(m) : m))
      } else if (needsFallback(o.material)) {
        if (!reported.has(o.material)) {
          reported.add(o.material)
          if (process.env.XRIFT_DEBUG) console.log(`[compat] ShaderMaterial → fallback: ${o.name || o.type}`)
        }
        o.material = fallbackFor(o.material)
      }
    })
  })
  return null
}
