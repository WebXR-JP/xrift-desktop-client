const { OpenXRSession } = require('../bin/xrift_openxr.node')
const s = new OpenXRSession({ appName: 'bench', resolutionScale: 1 })
const info = s.getInfo(); console.log(info.width, info.height)
const pitch = info.width * 2 * 4
const buf = new ArrayBuffer(pitch * info.height)
;(async () => {
  const t0 = Date.now(); let n = 0, tSync = 0, tAsync = 0
  while (Date.now() - t0 < 6000) {
    s.pollEvents()
    try { await s.waitFrame() } catch { await new Promise(r => setTimeout(r, 30)); continue }
    s.beginFrame(); const lv = s.locateViews()
    let a = performance.now(); s.submit(0, buf, 0, pitch); s.submit(1, buf, info.width * 4, pitch); tSync += performance.now() - a
    a = performance.now(); s.submitLatched(buf, 0, info.width * 4, pitch, lv.seq); tAsync += performance.now() - a
    s.endFrame(); n++
  }
  console.log({ n, sync: (tSync / n).toFixed(1), async: (tAsync / n).toFixed(1) })
  s.destroy(); process.exit(0)
})()
