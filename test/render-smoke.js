import '../src/env/dom.js'
import { installModuleHooks } from '../src/env/module-hooks.js'
import { installWebGPU, createRenderer, releaseWebGPU } from '../src/render/renderer.js'
import { writeFileSync } from 'node:fs'
installModuleHooks()
installWebGPU()
const THREE = await import('three')
const canvas = document.createElement('canvas')
canvas.width = 512; canvas.height = 512
const renderer = await createRenderer({ canvas })
renderer.setSize(512, 512, false)
const scene = new THREE.Scene()
scene.background = new THREE.Color(0x203040)
scene.add(new THREE.HemisphereLight(0xffffff, 0x444444, 2))
const dl = new THREE.DirectionalLight(0xffffff, 2); dl.position.set(2,3,1); scene.add(dl)
// テクスチャ付き（canvas 2D → CanvasTexture）
const c2 = document.createElement('canvas'); c2.width = 128; c2.height = 128
const ctx = c2.getContext('2d'); ctx.fillStyle = '#ff8800'; ctx.fillRect(0,0,128,128); ctx.fillStyle='#0044ff'; ctx.fillRect(0,0,64,64)
const tex = new THREE.CanvasTexture(c2); tex.colorSpace = THREE.SRGBColorSpace
const cube = new THREE.Mesh(new THREE.BoxGeometry(1,1,1), new THREE.MeshStandardMaterial({ map: tex }))
cube.rotation.set(0.5, 0.7, 0)
scene.add(cube)
const cam = new THREE.PerspectiveCamera(50, 1, 0.1, 100); cam.position.set(0, 0, 3)
renderer.render(scene, cam)
const ctxGPU = canvas.getContext('webgpu')
const texGPU = ctxGPU.getCurrentTexture()
const device = renderer.backend.device
const bpr = Math.ceil(512*4/256)*256
const buf = device.createBuffer({ size: bpr*512, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST })
const enc = device.createCommandEncoder(); enc.copyTextureToBuffer({ texture: texGPU }, { buffer: buf, bytesPerRow: bpr }, [512,512]); device.queue.submit([enc.finish()])
await buf.mapAsync(GPUMapMode.READ)
const raw = new Uint8Array(buf.getMappedRange()).slice()
buf.unmap()
console.log('format', texGPU.format, 'center px', [...raw.subarray((256*bpr)+256*4, (256*bpr)+256*4+4)])
const out = document.createElement('canvas'); out.width=512; out.height=512
const octx = out.getContext('2d'); const id = octx.createImageData(512,512)
const bgra = texGPU.format.startsWith('bgra')
for (let y=0;y<512;y++) for (let x=0;x<512;x++){ const s=y*bpr+x*4, d=(y*512+x)*4; id.data[d]=raw[s+(bgra?2:0)]; id.data[d+1]=raw[s+1]; id.data[d+2]=raw[s+(bgra?0:2)]; id.data[d+3]=255 }
octx.putImageData(id,0,0)
writeFileSync('test/render-smoke.png', Buffer.from(out.toDataURL().split(',')[1],'base64'))
console.log('ok')
renderer.dispose(); releaseWebGPU(); process.exit(0)
