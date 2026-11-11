const assert = require('node:assert/strict')
const { readFileSync } = require('node:fs')
const { runInNewContext } = require('node:vm')

async function check(stereoOffset = 0, stereoVertical = 0) {
  const eye = (x, yaw, fov) => ({
    position: [x, 1.65, 0],
    orientation: [0, Math.sin(yaw / 2), 0, Math.cos(yaw / 2)],
    fov,
  })
  const views = [
    eye(-0.032, -0.2, [-0.9, 0.6, 0.7, -0.8]),
    eye(0.032, 0.2, [-0.6, 0.9, 0.8, -0.7]),
  ]
  let started = false, exiting = false, nextFrame
  const bridge = {
    probe: () => ({ ok: true }),
    prepareGPU: async () => ({ ok: true }),
    createSession: () => ({ width: 64, height: 64, backend: 'openvr', stereoOffset, stereoVertical }),
    pollEvents: () => {
      if (exiting) return [{ type: 'exit' }]
      if (started) return []
      started = true
      return [{ type: 'begin' }, { type: 'state', detail: 'focused' }]
    },
    waitFrame: () => new Promise((resolve) => { nextFrame = resolve }),
    beginFrame() {},
    locateViews: () => ({ views, valid: true, positionTracked: true, seq: 1 }),
    syncInput: () => [],
    present() {},
    endFrame() {},
    requestExit() { exiting = true; nextFrame({ shouldRender: false }) },
    destroy() {},
  }
  const navigator = {}
  const window = { __xriftOpenXR: bridge }
  const module = { exports: {} }
  const context = { navigator, window, module, DOMException, EventTarget, Event, performance,
    setTimeout: (fn, ms) => setTimeout(fn, ms).unref(),
    MessageChannel: class { port1 = {}; port2 = {} },
  }
  runInNewContext(readFileSync(require.resolve('../electron/webxr-mainworld.cjs'), 'utf8'), context)
  module.exports()
  const session = await navigator.xr.requestSession('immersive-vr')
  const space = await session.requestReferenceSpace('unbounded')
  const gl = { canvas: { width: 128, height: 64 }, getContextAttributes: () => ({ antialias: false }) }
  session.updateRenderState({ baseLayer: new window.XRWebGLLayer(session, gl) })
  const result = new Promise((resolve) => {
    session.requestAnimationFrame((_time, frame) => resolve(frame.getViewerPose(space)))
  })
  nextFrame({ shouldRender: true })
  const pose = await result
  for (const [i, view] of pose.views.entries()) {
    const expectedSign = i === 0 ? -1 : 1
    assert.equal(view.eye, i === 0 ? 'left' : 'right')
    assert.equal(Math.sign(view.transform.orientation.y), expectedSign)
    const [left, right, up, down] = views[i].fov.map(Math.tan)
    const expectedProjectionCenter = (right + left) / (right - left) + 2 * expectedSign * stereoOffset
    assert.ok(Math.abs(view.projectionMatrix[8] - expectedProjectionCenter) < 1e-6)
    const expectedVerticalCenter = (up + down) / (up - down) + 2 * expectedSign * stereoVertical
    assert.ok(Math.abs(view.projectionMatrix[9] - expectedVerticalCenter) < 1e-6)
    assert.equal(session.renderState.baseLayer.getViewport(view).x, i * 64)
  }
  await session.end()
}

;(async () => {
  await check()
  await check(0.057, 0.005)
  console.log('WebXR stereo projection checks passed')
})().catch((error) => {
  console.error(error)
  process.exitCode = 1
})
