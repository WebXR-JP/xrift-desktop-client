// 読み戻しバッファ（両眼）を左右に並べた PNG にする
import { writeFileSync } from 'node:fs'
export function dumpEyes(file, { data, width, height, bytesPerRow, layerBytes, format }) {
  const raw = new Uint8Array(data)
  const out = document.createElement('canvas'); out.width = width * 2; out.height = height
  const ctx = out.getContext('2d'); const id = ctx.createImageData(width * 2, height)
  const bgra = format.startsWith('bgra')
  for (let eye = 0; eye < 2; eye++) for (let y = 0; y < height; y++) for (let x = 0; x < width; x++) {
    const s = eye * layerBytes + y * bytesPerRow + x * 4, d = (y * width * 2 + eye * width + x) * 4
    id.data[d] = raw[s + (bgra ? 2 : 0)]; id.data[d + 1] = raw[s + 1]; id.data[d + 2] = raw[s + (bgra ? 0 : 2)]; id.data[d + 3] = 255
  }
  ctx.putImageData(id, 0, 0)
  writeFileSync(file, Buffer.from(out.toDataURL().split(',')[1], 'base64'))
}
