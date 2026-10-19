import '../src/env/dom.js'
import { installModuleHooks } from '../src/env/module-hooks.js'
import { installWebGPU, createRenderer, releaseWebGPU } from '../src/render/renderer.js'
import { installWebXR } from '../src/xr/webxr-openxr.js'
installModuleHooks()
installWebGPU()
const { dumpEyes } = await import('./dump.js'); let dumped = 0
installWebXR({ resolutionScale: 0.5, verbose: true, forceRender: true, onReadback: (r) => { if (++dumped === 120) dumpEyes('test/xr-smoke.png', r) } })
const THREE = await import('three')
const canvas = document.createElement('canvas'); canvas.width = 640; canvas.height = 360
const renderer = await createRenderer({ canvas })
renderer.xr.enabled = true; renderer.toneMapping = THREE.ACESFilmicToneMapping
const scene = new THREE.Scene(); scene.background = new THREE.Color(0x335577)
scene.add(new THREE.HemisphereLight(0xffffff, 0x444444, 2))
const cube = new THREE.Mesh(new THREE.BoxGeometry(0.4,0.4,0.4), new THREE.MeshStandardMaterial({ color: 0xff8800 }))
cube.position.set(0, 1.5, -1.5); scene.add(cube)
const floor = new THREE.Mesh(new THREE.PlaneGeometry(10,10), new THREE.MeshStandardMaterial({ color: 0x777777 })); floor.rotation.x = -Math.PI/2; scene.add(floor)
const cam = new THREE.PerspectiveCamera(70, 16/9, 0.05, 100)
const c0 = renderer.xr.getController(0); scene.add(c0)
let frames = 0
renderer.setAnimationLoop((t, frame) => { cube.rotation.y = t/1000; renderer.render(scene, cam); if (frame) frames++ })
const session = await navigator.xr.requestSession('immersive-vr', { requiredFeatures: ['webgpu'], optionalFeatures: ['local-floor'] })
await renderer.xr.setSession(session)
console.log('presenting', renderer.xr.isPresenting)
const t0 = performance.now()
await new Promise(r => setTimeout(r, 6000))
console.log('xr frames rendered', frames, 'fps', (frames/((performance.now()-t0)/1000)).toFixed(1))
await session.end()
console.log('ended'); renderer.dispose(); releaseWebGPU(); process.exit(0)
