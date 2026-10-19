// OpenVR 経路の疎通テスト: 8 秒間、左眼=青 右眼=橙 を送る
const { OpenVRSession, probeOpenVR } = require('../bin/xrift_openxr.node')
console.log(probeOpenVR())
const s = new OpenVRSession({ resolutionScale: 1 })
const info = s.getInfo(); console.log(info)
const pitch = info.width * 8
const buf = new Uint8Array(pitch * info.height)
for (let y = 0; y < info.height; y++) for (let x = 0; x < info.width * 2; x++) { const o = y * pitch + x * 4; buf.set(x < info.width ? [40, 90, 220, 255] : [230, 140, 40, 255], o) }
;(async () => {
  const t0 = Date.now(); let n = 0, ok = 0, tw = 0, ts = 0
  while (Date.now() - t0 < 8000) {
    for (const e of s.pollEvents()) console.log('event', e)
    let a = performance.now(); const fs = await s.waitFrame(); tw += performance.now() - a
    s.beginFrame(); const lv = s.locateViews(); const inp = s.syncInput()
    if (n === 100) console.log('views', lv.valid, [...lv.views[0].position].map((v: number) => v.toFixed(2)), 'fov', [...lv.views[0].fov].map((v: number) => v.toFixed(2)), 'hands', inp.map((h: { handedness: string; connected: boolean; profile: string }) => `${h.handedness}:${h.connected}:${h.profile}`))
    if (fs.shouldRender) ok++
    a = performance.now(); s.submitLatched(buf.buffer, 0, info.width * 4, pitch, lv.seq); ts += performance.now() - a
    s.endFrame(); n++
  }
  console.log({ n, fps: (n / 8).toFixed(1), shouldRender: ok, wait: (tw / n).toFixed(1), submit: (ts / n).toFixed(1) })
  s.destroy(); process.exit(0)
})()
