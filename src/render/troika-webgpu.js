// troika-three-text（drei の <Text>）を WebGPU で描くための TSL 版マテリアル
//
// troika は onBeforeCompile で GLSL を差し込んだ「派生マテリアル」でグリフを描くが、
// WebGPURenderer は onBeforeCompile を使えないので、ただの四角が描かれてしまう。
// troika の頂点/フラグメント処理（troika-three-text の VERTEX_TRANSFORM / FRAGMENT_TRANSFORM）を
// TSL に移植した MeshBasicNodeMaterial を作り、Text メッシュの material として差し替える。
// ユニフォームの値は troika が派生マテリアルへ書いたものを毎回読み出す。

import {
  attribute,
  clamp,
  cos,
  float,
  floor,
  fwidth,
  length,
  max,
  mix,
  mod,
  positionGeometry,
  select,
  sin,
  smoothstep,
  texture,
  uniform,
  varying,
  vec2,
  vec3,
  vec4,
} from 'three/tsl'
import { MeshBasicNodeMaterial } from 'three/webgpu'
import { Color, Matrix3, Texture, Vector2, Vector4 } from 'three'

const nodeMaterials = new WeakMap()
const placeholderTexture = new Texture()

function u(derived, name, initial) {
  return uniform(initial).onObjectUpdate(() => {
    const v = derived.uniforms?.[name]?.value
    return typeof v === 'boolean' ? (v ? 1 : 0) : v
  })
}

function createNodeMaterial(derived, hasGlyphColors) {
  const base = derived.baseMaterial ?? derived
  const U = {
    texSize: u(derived, 'uTroikaSDFTextureSize', new Vector2()),
    glyphSize: u(derived, 'uTroikaSDFGlyphSize', 0),
    exponent: u(derived, 'uTroikaSDFExponent', 0),
    totalBounds: u(derived, 'uTroikaTotalBounds', new Vector4()),
    clipRect: u(derived, 'uTroikaClipRect', new Vector4()),
    edgeOffset: u(derived, 'uTroikaEdgeOffset', 0),
    fillOpacity: u(derived, 'uTroikaFillOpacity', 1),
    positionOffset: u(derived, 'uTroikaPositionOffset', new Vector2()),
    curveRadius: u(derived, 'uTroikaCurveRadius', 0),
    blurRadius: u(derived, 'uTroikaBlurRadius', 0),
    strokeWidth: u(derived, 'uTroikaStrokeWidth', 0),
    strokeColor: u(derived, 'uTroikaStrokeColor', new Color()),
    strokeOpacity: u(derived, 'uTroikaStrokeOpacity', 1),
    orient: u(derived, 'uTroikaOrient', new Matrix3()),
    useGlyphColors: u(derived, 'uTroikaUseGlyphColors', 1),
    // troika は Text.color を派生マテリアル自身の color に書く（未指定ならベースから継承）
    diffuse: uniform(new Color()).onObjectUpdate(() => derived.color),
    opacity: uniform(1).onObjectUpdate(() => derived.opacity ?? 1),
  }
  const sdfTexture = texture(placeholderTexture).onObjectUpdate(() => derived.uniforms?.uTroikaSDFTexture?.value ?? placeholderTexture)

  // ---------------- 頂点
  const rawBounds = attribute('aTroikaGlyphBounds', 'vec4')
  const glyphIndex = attribute('aTroikaGlyphIndex', 'float')
  const glyphColor = hasGlyphColors ? attribute('aTroikaGlyphColor', 'vec3') : vec3(255)

  const bounds = vec4(
    rawBounds.x.add(U.positionOffset.x),
    rawBounds.y.sub(U.positionOffset.y),
    rawBounds.z.add(U.positionOffset.x),
    rawBounds.w.sub(U.positionOffset.y),
  )
  const pad = U.edgeOffset.add(U.blurRadius)
  const outlineMin = bounds.xy.sub(pad)
  const outlineMax = bounds.zw.add(pad)
  const clippedMin = clamp(outlineMin, U.clipRect.xy, U.clipRect.zw)
  const clippedMax = clamp(outlineMax, U.clipRect.xy, U.clipRect.zw)
  const clippedXY = mix(clippedMin, clippedMax, positionGeometry.xy).sub(bounds.xy).div(bounds.zw.sub(bounds.xy))
  const pxy = mix(bounds.xy, bounds.zw, clippedXY)

  const rad = U.curveRadius
  const angle = pxy.x.div(select(rad.equal(0), float(1), rad))
  const curved = vec3(sin(angle).mul(rad), pxy.y, rad.sub(cos(angle).mul(rad)))
  const flat = vec3(pxy, positionGeometry.z)
  const localPos = U.orient.mul(select(rad.equal(0), flat, curved))

  const txCols = U.texSize.x.div(U.glyphSize)
  const perSquare = vec2(U.glyphSize, U.glyphSize).div(U.texSize)
  const cell = floor(glyphIndex.div(4))
  const txStart = perSquare.mul(vec2(mod(cell, txCols), floor(cell.div(txCols))))

  const vGlyphUV = varying(clippedXY, 'vTroikaGlyphUV')
  const vUVBounds = varying(vec4(txStart, txStart.add(perSquare)), 'vTroikaTextureUVBounds')
  const vChannel = varying(mod(glyphIndex, 4), 'vTroikaTextureChannel')
  const vDims = varying(vec2(bounds.z.sub(bounds.x), bounds.w.sub(bounds.y)), 'vTroikaGlyphDimensions')
  const vColor = varying(select(U.useGlyphColors.equal(1), glyphColor.div(255), vec3(U.diffuse)), 'vTroikaGlyphColor')

  // ---------------- フラグメント
  const aaDist = length(fwidth(vGlyphUV.mul(vDims))).mul(0.5)
  const halfTexel = float(0.5).div(U.glyphSize)
  const clampedUV = clamp(vGlyphUV, halfTexel, float(1).sub(halfTexel))
  const rgba = sdfTexture.sample(mix(vUVBounds.xy, vUVBounds.zw, clampedUV))
  const ch = floor(vChannel.add(0.5))
  const sdf = select(ch.equal(0), rgba.r, select(ch.equal(1), rgba.g, select(ch.equal(2), rgba.b, rgba.a)))
  const maxDim = max(vDims.x, vDims.y)
  const inside = sdf.greaterThan(0.5)
  const t = select(inside, float(1).sub(sdf), sdf)
  const absDist = float(1).sub(t.mul(2).pow(float(1).div(U.exponent))).mul(maxDim)
  const fragDist = absDist.mul(select(inside, float(-1), float(1))).add(length(vGlyphUV.sub(clampedUV).mul(vDims)))

  const aa = max(aaDist, U.blurRadius)
  // smoothstep(offset + aa, offset - aa, d) == 1 - smoothstep(offset - aa, offset + aa, d)
  const edgeAlpha = float(1).sub(smoothstep(U.edgeOffset.sub(aa), U.edgeOffset.add(aa), fragDist))

  const fillA = U.opacity.mul(U.fillOpacity)
  const hasStroke = U.strokeWidth.notEqual(0)
  const strokeRGB = select(hasStroke, vec3(U.strokeColor), vColor)
  const strokeA = select(hasStroke, U.strokeOpacity, fillA)
  const fillRGB = select(fillA.equal(0), strokeRGB, vColor)
  const k = smoothstep(U.strokeWidth.negate().sub(aaDist), U.strokeWidth.negate().add(aaDist), fragDist)
  const outRGB = mix(fillRGB, strokeRGB, k)
  const outA = mix(fillA, strokeA, k).mul(edgeAlpha)

  const m = new MeshBasicNodeMaterial()
  m.name = 'TroikaTextNodeMaterial'
  m.positionNode = localPos
  m.colorNode = vec4(outRGB, 1)
  m.opacityNode = outA
  m.alphaTest = 1e-4
  m.transparent = true
  m.side = base.side
  m.depthWrite = base.depthWrite
  m.depthTest = base.depthTest
  m.toneMapped = base.toneMapped
  m.fog = base.fog ?? true
  m.isTroikaTextNodeMaterial = true
  return m
}

function nodeMaterialFor(derived, geometry) {
  if (!derived?.isTroikaTextMaterial) return derived
  const hasGlyphColors = !!geometry?.attributes?.aTroikaGlyphColor
  let entry = nodeMaterials.get(derived)
  if (!entry || entry.hasGlyphColors !== hasGlyphColors) {
    entry = { m: createNodeMaterial(derived, hasGlyphColors), hasGlyphColors }
    nodeMaterials.set(derived, entry)
  }
  const m = entry.m
  m.side = derived.side
  m.visible = derived.visible !== false
  m.depthWrite = derived.depthWrite
  m.depthTest = derived.depthTest
  m.polygonOffset = derived.polygonOffset
  m.polygonOffsetFactor = derived.polygonOffsetFactor
  m.polygonOffsetUnits = derived.polygonOffsetUnits
  m.renderOrder = derived.renderOrder
  return m
}

function findMaterialAccessor(obj) {
  for (let p = Object.getPrototypeOf(obj); p; p = Object.getPrototypeOf(p)) {
    const d = Object.getOwnPropertyDescriptor(p, 'material')
    if (d?.get) return d
  }
  return null
}

/** troika の Text メッシュを WebGPU で描けるようにする（何度呼んでもよい） */
export function installTroikaWebGPU(text) {
  if (text.__xriftTroikaWebGPU) return
  const acc = findMaterialAccessor(text)
  if (!acc) return
  text.__xriftTroikaWebGPU = true
  const getDerived = () => acc.get.call(text)
  Object.defineProperty(text, 'material', {
    configurable: true,
    get() {
      const d = getDerived()
      return Array.isArray(d) ? d.map((x) => nodeMaterialFor(x, text.geometry)) : nodeMaterialFor(d, text.geometry)
    },
    set(v) {
      acc.set.call(text, v)
    },
  })
  // three は描画するマテリアル（= ノードマテリアル）を onBeforeRender に渡すので、
  // troika が派生マテリアルのユニフォームを更新できるよう自分で呼ぶ
  text.onBeforeRender = function () {
    this.sync()
    const d = getDerived()
    for (const m of Array.isArray(d) ? d : [d]) {
      if (m?.isTroikaTextMaterial) this._prepareForRender(m)
    }
  }
}
