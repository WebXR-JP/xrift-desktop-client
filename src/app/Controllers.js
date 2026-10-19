// コントローラ: 見た目（簡易モデル）+ レーザー + XRift の Interactable 操作
//
// トリガーを引くと、レーザーが当たっている <Interactable> の onInteract(id) を呼ぶ
// （XRift 本家のクリック操作と同じ。対象は LAYERS.INTERACTABLE のレイヤー）。

import { createElement as h, useEffect, useMemo, useRef } from 'react'
import { useFrame, useThree } from '@react-three/fiber'
import * as THREE from 'three'
import { LAYERS } from '@xrift/world-components'

const RAY_LENGTH = 6

function findInteractable(object) {
  for (let node = object; node; node = node.parent) {
    const ud = node.userData
    if (typeof ud?.onInteract === 'function' && ud.enabled !== false) return node
  }
  return null
}

function Controller({ index, rigRef }) {
  const { gl, scene } = useThree()
  const parts = useMemo(() => {
    const ray = gl.xr.getController(index)
    const grip = gl.xr.getControllerGrip(index)

    const body = new THREE.Mesh(
      new THREE.CylinderGeometry(0.018, 0.024, 0.12, 16).rotateX(Math.PI / 2),
      new THREE.MeshStandardMaterial({ color: 0x2b2f36, roughness: 0.6, metalness: 0.1 }),
    )
    body.position.set(0, -0.01, 0.04)
    const ring = new THREE.Mesh(
      new THREE.TorusGeometry(0.035, 0.006, 8, 24),
      new THREE.MeshStandardMaterial({ color: 0x5aa9ff, emissive: 0x1a4a88, roughness: 0.4 }),
    )
    ring.position.set(0, 0.0, -0.02)
    grip.add(body, ring)

    const laserGeom = new THREE.BufferGeometry().setFromPoints([new THREE.Vector3(0, 0, 0), new THREE.Vector3(0, 0, -1)])
    const laserMat = new THREE.LineBasicMaterial({ color: 0x9cd2ff, transparent: true, opacity: 0.8 })
    const laser = new THREE.Line(laserGeom, laserMat)
    laser.scale.z = RAY_LENGTH
    const dot = new THREE.Mesh(new THREE.SphereGeometry(0.012, 12, 8), new THREE.MeshBasicMaterial({ color: 0xffffff }))
    dot.visible = false
    ray.add(laser)
    // 各オブジェクトがレイキャスト対象にならないように
    for (const o of [body, ring, laser, dot]) o.raycast = () => {}
    return { ray, grip, laser, laserMat, dot }
  }, [gl, index])

  const hoverRef = useRef(null)
  const sourceRef = useRef(null)
  const raycaster = useMemo(() => {
    const r = new THREE.Raycaster()
    r.layers.set(LAYERS.INTERACTABLE)
    r.far = RAY_LENGTH
    return r
  }, [])

  useEffect(() => {
    const rig = rigRef.current ?? scene
    rig.add(parts.ray, parts.grip)
    scene.add(parts.dot)
    const onSelect = () => {
      const target = hoverRef.current
      if (!target) return
      target.userData.onInteract(target.userData.id ?? '')
      sourceRef.current?.gamepad?.hapticActuators?.[0]?.pulse(0.6, 40)
    }
    const onConnected = (e) => { sourceRef.current = e.data }
    const onDisconnected = () => { sourceRef.current = null }
    parts.ray.addEventListener('selectstart', onSelect)
    parts.ray.addEventListener('connected', onConnected)
    parts.ray.addEventListener('disconnected', onDisconnected)
    return () => {
      parts.ray.removeEventListener('selectstart', onSelect)
      parts.ray.removeEventListener('connected', onConnected)
      parts.ray.removeEventListener('disconnected', onDisconnected)
      rig.remove(parts.ray, parts.grip)
      scene.remove(parts.dot)
    }
  }, [parts, rigRef, scene])

  const _o = useMemo(() => new THREE.Vector3(), [])
  const _d = useMemo(() => new THREE.Vector3(), [])
  const _q = useMemo(() => new THREE.Quaternion(), [])
  useFrame(() => {
    const { ray, laser, laserMat, dot } = parts
    if (!ray.visible) {
      hoverRef.current = null
      dot.visible = false
      return
    }
    ray.updateMatrixWorld(true)
    ray.getWorldPosition(_o)
    ray.getWorldQuaternion(_q)
    _d.set(0, 0, -1).applyQuaternion(_q)
    raycaster.set(_o, _d)
    const hits = raycaster.intersectObjects(scene.children, true)
    let hover = null
    let dist = RAY_LENGTH
    for (const hit of hits) {
      const target = findInteractable(hit.object)
      if (target) {
        hover = target
        dist = hit.distance
        dot.position.copy(hit.point)
        break
      }
    }
    hoverRef.current = hover
    dot.visible = !!hover
    laser.scale.z = dist
    laserMat.color.set(hover ? 0x7dffb0 : 0x9cd2ff)
  })

  return null
}

export function Controllers({ rigRef }) {
  return h('group', null, h(Controller, { index: 0, rigRef }), h(Controller, { index: 1, rigRef }))
}
