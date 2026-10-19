// CPU 上の極小 WebGL コンテキスト
//
// drei の <Text>（troika-three-text）は SDF のアトラスを WebGL キャンバスに持ち、
// 「テクスチャをビューポート矩形へコピー（colorMask 付き）」と readPixels しか使わない
// （SDF 自体は JS 実装へフォールバックさせる）。その範囲だけを RGBA バッファで再現する。
// それ以外の呼び出しはすべて何もしない。

let enumCounter = 0x9000
const enumCache = new Map()

export class SoftGLContext {
  constructor(canvas) {
    this.canvas = canvas
    this.drawingBufferWidth = canvas.width
    this.drawingBufferHeight = canvas.height
    this._buf = new Uint8Array(Math.max(1, canvas.width) * Math.max(1, canvas.height) * 4)
    this._bw = canvas.width
    this._bh = canvas.height
    this._program = null
    this._textures = new Map()
    this._boundTexture = null
    this._viewport = [0, 0, canvas.width, canvas.height]
    this._mask = [true, true, true, true]
    this._framebuffer = null
    this.version = 1
    return new Proxy(this, {
      get(target, prop) {
        if (prop in target) {
          const v = target[prop]
          return typeof v === 'function' ? v.bind(target) : v
        }
        if (typeof prop !== 'string') return undefined
        if (/^[A-Z0-9_]+$/.test(prop)) {
          if (!enumCache.has(prop)) enumCache.set(prop, enumCounter++)
          return enumCache.get(prop)
        }
        return () => null
      },
    })
  }

  _ensureSize() {
    const w = this.canvas.width
    const h = this.canvas.height
    if (w !== this._bw || h !== this._bh) {
      // キャンバスのリサイズで描画バッファは消える（ブラウザと同じ）
      this._buf = new Uint8Array(Math.max(1, w) * Math.max(1, h) * 4)
      this._bw = w
      this._bh = h
    }
    this.drawingBufferWidth = w
    this.drawingBufferHeight = h
  }

  /** ブラウザのキャンバス画像と同じ向き（上の行から）で RGBA を返す */
  _getPixels() {
    this._ensureSize()
    const w = this._bw
    const h = this._bh
    const out = new Uint8Array(w * h * 4)
    const row = w * 4
    for (let y = 0; y < h; y++) out.set(this._buf.subarray(y * row, y * row + row), (h - 1 - y) * row)
    return { width: w, height: h, data: out }
  }

  getContextAttributes() { return { alpha: true, antialias: false, depth: false, premultipliedAlpha: false, preserveDrawingBuffer: true } }
  isContextLost() { return false }
  getExtension(name) {
    if (name === 'ANGLE_instanced_arrays') return { VERTEX_ATTRIB_ARRAY_DIVISOR_ANGLE: 0x88fe, vertexAttribDivisorANGLE() {}, drawArraysInstancedANGLE() {}, drawElementsInstancedANGLE() {} }
    return null
  }
  getSupportedExtensions() { return [] }
  getParameter() { return 0 }
  getError() { return 0 }

  createShader(type) { return { type, source: '' } }
  shaderSource(sh, src) { sh.source = src }
  compileShader() {}
  getShaderParameter() { return true }
  getShaderInfoLog() { return '' }
  createProgram() { return { shaders: [] } }
  attachShader(p, sh) { p.shaders.push(sh) }
  linkProgram() {}
  getProgramParameter() { return true }
  getProgramInfoLog() { return '' }
  useProgram(p) { this._program = p }
  getAttribLocation() { return 0 }
  getUniformLocation(_p, name) { return { name } }

  createTexture() { const t = { data: null, w: 0, h: 0 }; this._textures.set(t, t); return t }
  bindTexture(_target, t) { this._boundTexture = t }
  deleteTexture() {}
  texImage2D(...args) {
    // texImage2D(target, level, internalformat, width, height, border, format, type, pixels)
    if (args.length >= 9 && this._boundTexture) {
      const [, , , w, h, , , , pixels] = args
      this._boundTexture.w = w
      this._boundTexture.h = h
      this._boundTexture.data = pixels ? new Uint8Array(pixels.buffer, pixels.byteOffset, pixels.byteLength) : null
    }
  }

  bindFramebuffer(_t, fb) { this._framebuffer = fb }
  viewport(x, y, w, h) { this._viewport = [x, y, w, h] }
  colorMask(r, g, b, a) { this._mask = [!!r, !!g, !!b, !!a] }
  clearColor() {}
  clear() {
    this._ensureSize()
    this._buf.fill(0)
  }

  drawArrays() {
    // 対応するのは「テクスチャをそのままコピー」のプログラムだけ（フレームバッファ無しの描画）
    this._ensureSize()
    if (this._framebuffer) return
    const frag = this._program?.shaders?.find((s) => /gl_FragColor|out\s+vec4/.test(s.source))?.source ?? ''
    // webgl-sdf-generator の copyTexFragment: gl_FragColor=texture2D(tex,vUV);
    if (!/gl_FragColor\s*=\s*texture2D\(\s*\w+\s*,\s*vUV\s*\)\s*;\s*\}\s*$/.test(frag)) return
    const tex = this._boundTexture
    if (!tex?.data) return
    const [vx, vy, vw, vh] = this._viewport
    const w = Math.min(vw, tex.w)
    const h = Math.min(vh, tex.h)
    const bw = this._bw
    const m = this._mask
    for (let y = 0; y < h; y++) {
      const dy = vy + y
      if (dy < 0 || dy >= this._bh) continue
      for (let x = 0; x < w; x++) {
        const dx = vx + x
        if (dx < 0 || dx >= bw) continue
        const s = (y * tex.w + x) * 4
        const d = (dy * bw + dx) * 4
        if (m[0]) this._buf[d] = tex.data[s]
        if (m[1]) this._buf[d + 1] = tex.data[s + 1]
        if (m[2]) this._buf[d + 2] = tex.data[s + 2]
        if (m[3]) this._buf[d + 3] = tex.data[s + 3]
      }
    }
  }

  readPixels(x, y, w, h, _format, _type, out) {
    this._ensureSize()
    const bw = this._bw
    for (let row = 0; row < h; row++) {
      const sy = y + row
      if (sy < 0 || sy >= this._bh) continue
      const src = (sy * bw + x) * 4
      out.set(this._buf.subarray(src, src + w * 4), row * w * 4)
    }
  }
}
