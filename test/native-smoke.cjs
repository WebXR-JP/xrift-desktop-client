// ネイティブ単体の疎通テスト: 5 秒間、左眼=青 右眼=橙 の単色を送る
const { OpenXRSession } = require('../bin/xrift_openxr.node')
const s = new OpenXRSession({ appName: 'XRift smoke test', resolutionScale: 0.5 })
const info = s.getInfo()
console.log(info)
const bufs = [0, 1].map((eye) => {
  const b = new Uint8Array(info.width * info.height * 4)
  const c = eye === 0 ? [40, 90, 220, 255] : [230, 140, 40, 255]
  if (info.colorFormat === 'bgra8unorm') [c[0], c[2]] = [c[2], c[0]]
  for (let i = 0; i < b.length; i += 4) b.set(c, i)
  return b
})
let frames = 0, submitted = 0
const t0 = Date.now()
;(async () => {
  while (Date.now() - t0 < 8000) {
    for (const e of s.pollEvents()) console.log('event', e)
    let fs
    try { fs = await s.waitFrame() } catch (e) { await new Promise(r => setTimeout(r, 50)); continue }
    s.beginFrame()
    const v = s.locateViews()
    if (frames === 30) { console.log('views', JSON.stringify(v)); console.log('input', s.syncInput()) }
    if (fs.shouldRender) { s.submit(0, bufs[0]); s.submit(1, bufs[1]) }
    if (s.endFrame()) submitted++
    frames++
  }
  console.log({ frames, submitted, fps: frames / ((Date.now() - t0) / 1000) })
  s.requestExit()
  const t1 = Date.now()
  while (Date.now() - t1 < 2000) { for (const e of s.pollEvents()) console.log('event', e); await new Promise(r => setTimeout(r, 20)) }
  s.destroy()
})()
