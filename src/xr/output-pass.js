// XR 用の出力変換（トーンマッピング + sRGB エンコード + 8bit 量子化）
//
// three.js の WebGPU 出力パス（中間バッファ → 外部テクスチャ）は XRGPUBinding の配列テクスチャに
// 正しく書けないため、XR 中は three にはリニア HDR（rgba16float）のまま両眼を描かせ、
// ここでコンピュートシェーダを使って OpenXR スワップチェーン（*_SRGB）向けの 8bit 値に変換する。
// トーンマッピングの式は three.js（TSL の ToneMappingFunctions）と同じもの。

const WGSL = /* wgsl */ `
struct Params {
  width: u32,
  height: u32,
  rowWords: u32,
  layerWords: u32,
  toneMapping: u32,
  exposure: f32,
  bgra: u32,
  encodeSRGB: u32,
}

@group(0) @binding(0) var src: texture_2d_array<f32>;
@group(0) @binding(1) var<storage, read_write> dst: array<u32>;
@group(0) @binding(2) var<uniform> p: Params;

fn rrtAndOdtFit(v: vec3f) -> vec3f {
  let a = v * (v + 0.0245786) - 0.000090537;
  let b = v * (0.983729 * v + 0.4329510) + 0.238081;
  return a / b;
}

fn acesFilmic(c0: vec3f) -> vec3f {
  let inMat = mat3x3f(
    vec3f(0.59719, 0.07600, 0.02840),
    vec3f(0.35458, 0.90834, 0.13383),
    vec3f(0.04823, 0.01566, 0.83777));
  let outMat = mat3x3f(
    vec3f(1.60475, -0.10208, -0.00327),
    vec3f(-0.53108, 1.10813, -0.07276),
    vec3f(-0.07367, -0.00605, 1.07602));
  var c = c0 * (p.exposure / 0.6);
  c = inMat * c;
  c = rrtAndOdtFit(c);
  c = outMat * c;
  return clamp(c, vec3f(0.0), vec3f(1.0));
}

fn agxContrast(x: vec3f) -> vec3f {
  let x2 = x * x;
  let x4 = x2 * x2;
  return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

fn agx(c0: vec3f) -> vec3f {
  let srgbToRec2020 = mat3x3f(vec3f(0.6274, 0.0691, 0.0164), vec3f(0.3293, 0.9195, 0.0880), vec3f(0.0433, 0.0113, 0.8956));
  let rec2020ToSrgb = mat3x3f(vec3f(1.6605, -0.1246, -0.0182), vec3f(-0.5876, 1.1329, -0.1006), vec3f(-0.0728, -0.0083, 1.1187));
  let inset = mat3x3f(
    vec3f(0.856627153315983, 0.137318972929847, 0.11189821299995),
    vec3f(0.0951212405381588, 0.761241990602591, 0.0767994186031903),
    vec3f(0.0482516061458583, 0.101439036467562, 0.811302368396859));
  let outset = mat3x3f(
    vec3f(1.1271005818144368, -0.1413297634984383, -0.14132976349843826),
    vec3f(-0.11060664309660323, 1.157823702216272, -0.11060664309660294),
    vec3f(-0.016493938717834573, -0.016493938717834257, 1.2519364065950405));
  let minEv = -12.47393;
  let maxEv = 4.026069;
  var c = c0 * p.exposure;
  c = srgbToRec2020 * c;
  c = inset * c;
  c = max(c, vec3f(1e-10));
  c = log2(c);
  c = (c - minEv) / (maxEv - minEv);
  c = clamp(c, vec3f(0.0), vec3f(1.0));
  c = agxContrast(c);
  c = outset * c;
  c = pow(max(vec3f(0.0), c), vec3f(2.2));
  c = rec2020ToSrgb * c;
  return clamp(c, vec3f(0.0), vec3f(1.0));
}

fn neutral(c0: vec3f) -> vec3f {
  let startCompression = 0.8 - 0.04;
  let desaturation = 0.15;
  var c = c0 * p.exposure;
  let x = min(c.r, min(c.g, c.b));
  let offset = select(0.04, x - 6.25 * x * x, x < 0.08);
  c = c - offset;
  let peak = max(c.r, max(c.g, c.b));
  if (peak < startCompression) { return c; }
  let d = 1.0 - startCompression;
  let newPeak = 1.0 - d * d / (peak + d - startCompression);
  c = c * (newPeak / peak);
  let g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
  return mix(c, vec3f(newPeak), g);
}

fn cineon(c0: vec3f) -> vec3f {
  var c = max(vec3f(0.0), c0 * p.exposure - 0.004);
  let r = (c * (6.2 * c + 0.5)) / (c * (6.2 * c + 1.7) + 0.06);
  return pow(r, vec3f(2.2));
}

fn toneMap(c: vec3f) -> vec3f {
  switch (p.toneMapping) {
    case 1u: { return c * p.exposure; }
    case 2u: { let e = c * p.exposure; return e / (vec3f(1.0) + e); }
    case 3u: { return cineon(c); }
    case 4u: { return acesFilmic(c); }
    case 6u: { return agx(c); }
    case 7u: { return neutral(c); }
    default: { return c; }
  }
}

fn srgbEncode(c: vec3f) -> vec3f {
  let lo = c * 12.92;
  let hi = 1.055 * pow(c, vec3f(1.0 / 2.4)) - 0.055;
  return select(hi, lo, c <= vec3f(0.0031308));
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) id: vec3u) {
  if (id.x >= p.width || id.y >= p.height) { return; }
  var c = textureLoad(src, vec2u(id.xy), id.z, 0).rgb;
  c = clamp(toneMap(max(c, vec3f(0.0))), vec3f(0.0), vec3f(1.0));
  if (p.encodeSRGB != 0u) { c = srgbEncode(c); }
  var q = vec3u(round(c * 255.0));
  if (p.bgra != 0u) { q = q.bgr; }
  dst[id.z * p.layerWords + id.y * p.rowWords + id.x] = q.r | (q.g << 8u) | (q.b << 16u) | (255u << 24u);
}
`

export class XROutputPass {
  constructor(device) {
    this.device = device
    this.module = device.createShaderModule({ label: 'xr-output', code: WGSL })
    this.pipeline = device.createComputePipeline({ label: 'xr-output', layout: 'auto', compute: { module: this.module, entryPoint: 'main' } })
    this.params = device.createBuffer({ label: 'xr-output-params', size: 32, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST })
    this.storage = null
    this.bindGroup = null
    this.boundTexture = null
  }

  /** src（2 層配列テクスチャ）を変換してエンコーダに積み、読み戻し用バッファへコピーする */
  encode(encoder, srcTexture, readbackBuffer, { width, height, bytesPerRow, toneMapping, exposure, bgra, encodeSRGB }) {
    const layerBytes = bytesPerRow * height
    const size = layerBytes * 2
    if (!this.storage || this.storage.size !== size) {
      this.storage?.destroy()
      this.storage = this.device.createBuffer({ label: 'xr-output-storage', size, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC })
      this.bindGroup = null
    }
    if (!this.bindGroup || this.boundTexture !== srcTexture) {
      this.boundTexture = srcTexture
      this.bindGroup = this.device.createBindGroup({
        layout: this.pipeline.getBindGroupLayout(0),
        entries: [
          { binding: 0, resource: srcTexture.createView({ dimension: '2d-array' }) },
          { binding: 1, resource: { buffer: this.storage } },
          { binding: 2, resource: { buffer: this.params } },
        ],
      })
    }
    const u = new ArrayBuffer(32)
    const dv = new DataView(u)
    dv.setUint32(0, width, true)
    dv.setUint32(4, height, true)
    dv.setUint32(8, bytesPerRow / 4, true)
    dv.setUint32(12, layerBytes / 4, true)
    dv.setUint32(16, toneMapping >>> 0, true)
    dv.setFloat32(20, exposure, true)
    dv.setUint32(24, bgra ? 1 : 0, true)
    dv.setUint32(28, encodeSRGB ? 1 : 0, true)
    this.device.queue.writeBuffer(this.params, 0, u)

    const pass = encoder.beginComputePass({ label: 'xr-output' })
    pass.setPipeline(this.pipeline)
    pass.setBindGroup(0, this.bindGroup)
    pass.dispatchWorkgroups(Math.ceil(width / 8), Math.ceil(height / 8), 2)
    pass.end()
    encoder.copyBufferToBuffer(this.storage, 0, readbackBuffer, 0, size)
  }

  destroy() {
    this.storage?.destroy()
    this.params?.destroy()
  }
}
