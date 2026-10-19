// shouldRender に関係なく毎フレーム画像を出したとき、セッションが VISIBLE / FOCUSED に進むか
const { OpenXRSession } = require('../bin/xrift_openxr.node')
const s = new OpenXRSession({ appName: 'state test', resolutionScale: 0.5 })
const info = s.getInfo(); const pitch = info.width * 8
const buf = new Uint8Array(pitch * info.height).fill(128).buffer
let n = 0, rendered = 0
;(async () => {
  const t0 = Date.now()
  while (Date.now() - t0 < 10000) {
    for (const e of s.pollEvents()) console.log(((Date.now() - t0) / 1000).toFixed(2), e.type, e.detail ?? '')
    let fs; try { fs = await s.waitFrame() } catch { await new Promise(r => setTimeout(r, 20)); continue }
    s.beginFrame(); const lv = s.locateViews()
    if (fs.shouldRender) rendered++
    s.submitLatched(buf, 0, info.width * 4, pitch, lv.seq)
    s.endFrame(); n++
  }
  console.log({ n, shouldRenderFrames: rendered })
  s.requestExit(); const t1 = Date.now(); while (Date.now() - t1 < 1500) { s.pollEvents(); await new Promise(r => setTimeout(r, 20)) }
  s.destroy(); process.exit(0)
})()
