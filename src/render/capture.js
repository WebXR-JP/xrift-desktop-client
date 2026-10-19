// キャンバス（WebGPU のシム）の中身を PNG に保存する（デバッグ / --screenshot 用）
import { writeFileSync } from 'node:fs'

export async function captureCanvasPNG(renderer, canvas, file) {
  const device = renderer.backend.device
  const tex = canvas.getContext('webgpu').getCurrentTexture()
  const w = tex.width
  const h = tex.height
  const bpr = Math.ceil((w * 4) / 256) * 256
  const buf = device.createBuffer({ size: bpr * h, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST })
  const enc = device.createCommandEncoder()
  enc.copyTextureToBuffer({ texture: tex }, { buffer: buf, bytesPerRow: bpr }, [w, h])
  device.queue.submit([enc.finish()])
  await buf.mapAsync(GPUMapMode.READ)
  const raw = new Uint8Array(buf.getMappedRange()).slice()
  buf.unmap()
  buf.destroy()
  const bgra = tex.format.startsWith('bgra')
  const out = document.createElement('canvas')
  out.width = w
  out.height = h
  const ctx = out.getContext('2d')
  const id = ctx.createImageData(w, h)
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const s = y * bpr + x * 4
      const d = (y * w + x) * 4
      id.data[d] = raw[s + (bgra ? 2 : 0)]
      id.data[d + 1] = raw[s + 1]
      id.data[d + 2] = raw[s + (bgra ? 0 : 2)]
      id.data[d + 3] = 255
    }
  }
  ctx.putImageData(id, 0, 0)
  writeFileSync(file, Buffer.from(out.toDataURL('image/png').split(',')[1], 'base64'))
}
