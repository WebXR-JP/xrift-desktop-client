// コントローラの「生の姿勢」と aim（tip）の向きの差を表示する
const { OpenVRSession } = require('../bin/xrift_openxr.node')
const s = new OpenVRSession({ resolutionScale: 0.5 })
const fwd = (q) => { const [x, y, z, w] = q; return [-(2 * (x * z + w * y)), -(2 * (y * z - w * x)), -(1 - 2 * (x * x + y * y))] }
;(async () => {
  for (let i = 0; i < 90; i++) { s.pollEvents(); await s.waitFrame(); s.beginFrame(); s.locateViews(); const inp = s.syncInput()
    if (i === 89) for (const h of inp) if (h.connected) { const a = fwd(h.aim.orientation), g = fwd(h.grip.orientation); const ang = Math.acos(Math.max(-1, Math.min(1, a[0]*g[0]+a[1]*g[1]+a[2]*g[2]))) * 180 / Math.PI; console.log(h.handedness, h.profile.split('/').pop(), 'aim fwd', a.map(v => v.toFixed(2)).join(','), 'grip fwd', g.map(v => v.toFixed(2)).join(','), 'angle', ang.toFixed(1) + '°') }
    s.endFrame() }
  s.destroy(); process.exit(0)
})()
