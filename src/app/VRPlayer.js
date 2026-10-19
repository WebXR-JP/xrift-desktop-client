// VR プレイヤー: XR の原点（リグ）+ 物理カプセル + 移動 / スナップターン / ジャンプ
//
// - カメラはリグ（group）の子。three の XRManager は camera.parent を XR 原点として扱うので、
//   リグを動かす = プレイヤーの移動になる
// - 体（Rapier のカプセル）は常に頭の真下にいるように追従させる（ルームスケールの歩行も衝突する）
// - 左スティック: 頭の向き基準で移動 / 右スティック左右: 30° スナップターン / A ボタン: ジャンプ
//   右スティック下: 180° ターン / 左メニュー or Y: スポーン地点に戻る

import { createElement as h, useEffect, useRef } from 'react'
import { useFrame, useThree } from '@react-three/fiber'
import { CapsuleCollider, RigidBody } from '@react-three/rapier'
import { Vector3 } from 'three'
import { useSpawnPoint } from '@xrift/world-components'

export const PLAYER_HALF_HEIGHT = 0.4
export const PLAYER_RADIUS = 0.4
const FEET_OFFSET = PLAYER_HALF_HEIGHT + PLAYER_RADIUS
const DESKTOP_EYE_HEIGHT = 1.44
const SNAP_ANGLE = Math.PI / 6
const STICK_DEADZONE = 0.15
const MAX_FOLLOW_SPEED = 4 // ルームスケール追従の上限 [m/s]

const _head = new Vector3()
const _fwd = new Vector3()
const _right = new Vector3()

function axes(src) {
  const a = src?.gamepad?.axes
  return a ? [a[2] ?? 0, a[3] ?? 0] : [0, 0]
}

export function VRPlayer({ moveSpeed = 3, jumpVelocity = 4.5, respawnY = -10, spawnPosition = [0, 0, 0], movementRef, teleportRef, rigRef: externalRigRef }) {
  const rigRef = useRef(null)
  const bodyRef = useRef(null)
  const { camera, gl } = useThree()
  const spawnPoint = useSpawnPoint()
  const state = useRef({ snapArmed: true, jumpPrev: false, resetPrev: false, grounded: false, yaw: 0 })

  // カメラをリグにぶら下げる
  useEffect(() => {
    const rig = rigRef.current
    if (!rig) return
    if (externalRigRef) externalRigRef.current = rig
    rig.add(camera)
    if (!gl.xr.isPresenting) camera.position.set(0, DESKTOP_EYE_HEIGHT, 0)
    camera.rotation.set(0, 0, 0)
    return () => {
      rig.remove(camera)
    }
  }, [camera, gl])

  const placeAt = (pos, yawDeg) => {
    const body = bodyRef.current
    if (!body) return
    body.setTranslation({ x: pos[0], y: pos[1] + FEET_OFFSET + 0.05, z: pos[2] }, true)
    body.setLinvel({ x: 0, y: 0, z: 0 }, true)
    if (yawDeg !== undefined) {
      // 頭が向いている方向を考慮して、プレイヤーが yaw の方向を向くようにする
      const headYaw = gl.xr.isPresenting ? Math.atan2(-camera.matrix.elements[8], -camera.matrix.elements[10]) : 0
      state.current.yaw = (yawDeg * Math.PI) / 180 - headYaw
    }
  }

  const spawnRef = useRef(null)
  spawnRef.current = spawnPoint ? { position: spawnPoint.position, yaw: spawnPoint.yaw ?? 0 } : { position: spawnPosition, yaw: 0 }

  useEffect(() => {
    if (spawnPoint) placeAt(spawnPoint.position, spawnPoint.yaw ?? 0)
  }, [spawnPoint])

  useEffect(() => {
    if (teleportRef) teleportRef.current = (dest) => placeAt(dest.position, dest.yaw)
  })

  useFrame((_, delta) => {
    const body = bodyRef.current
    const rig = rigRef.current
    if (!body || !rig) return
    const dt = Math.min(Math.max(delta, 1e-3), 0.1)
    const st = state.current
    const session = gl.xr.getSession?.()
    const sources = session ? session.inputSources : []
    const left = sources.find((s) => s.handedness === 'left')
    const right = sources.find((s) => s.handedness === 'right')

    // ---- スナップターン（右スティック左右）/ 180° ターン（右スティック下）
    const [rx, ry] = axes(right)
    if (st.snapArmed && Math.abs(rx) > 0.7) {
      st.yaw += rx > 0 ? -SNAP_ANGLE : SNAP_ANGLE
      st.snapArmed = false
    } else if (st.snapArmed && ry > 0.8) {
      st.yaw += Math.PI
      st.snapArmed = false
    } else if (Math.abs(rx) < 0.3 && ry < 0.3) {
      st.snapArmed = true
    }

    // ---- リスポーン（左 Y ボタン）
    const resetPressed = !!left?.gamepad?.buttons[5]?.pressed
    if (resetPressed && !st.resetPrev) placeAt(spawnRef.current.position, spawnRef.current.yaw)
    st.resetPrev = resetPressed

    rig.rotation.set(0, st.yaw, 0)
    rig.updateMatrixWorld()

    // 頭のリグ内オフセット（水平）をワールドに回したもの
    const hx = camera.position.x
    const hz = camera.position.z
    const c = Math.cos(st.yaw)
    const s = Math.sin(st.yaw)
    const offX = c * hx + s * hz
    const offZ = -s * hx + c * hz

    // ---- 移動（左スティック、頭の向き基準）
    const [lx, ly] = axes(left)
    const mag = Math.hypot(lx, ly)
    camera.getWorldDirection(_fwd)
    _fwd.y = 0
    if (_fwd.lengthSq() < 1e-6) _fwd.set(0, 0, -1)
    _fwd.normalize()
    _right.set(-_fwd.z, 0, _fwd.x)
    let vx = 0
    let vz = 0
    if (mag > STICK_DEADZONE) {
      const k = (Math.min(1, (mag - STICK_DEADZONE) / (1 - STICK_DEADZONE)) * moveSpeed) / mag
      vx = (_right.x * lx - _fwd.x * ly) * k
      vz = (_right.z * lx - _fwd.z * ly) * k
    }

    // ---- ルームスケール追従: 体を頭の真下へ
    const p = body.translation()
    rig.getWorldPosition(_head)
    const headWX = _head.x + offX
    const headWZ = _head.z + offZ
    let fx = (headWX - p.x) / dt
    let fz = (headWZ - p.z) / dt
    const fl = Math.hypot(fx, fz)
    if (fl > MAX_FOLLOW_SPEED) {
      fx *= MAX_FOLLOW_SPEED / fl
      fz *= MAX_FOLLOW_SPEED / fl
    }

    // ---- ジャンプ（右 A ボタン）
    const v = body.linvel()
    let vy = v.y
    const jumpPressed = !!right?.gamepad?.buttons[4]?.pressed
    if (jumpPressed && !st.jumpPrev && Math.abs(v.y) < 0.5) vy = jumpVelocity
    st.jumpPrev = jumpPressed

    body.setLinvel({ x: vx + fx, y: vy, z: vz + fz }, true)

    // ---- リグを体に合わせる（頭が体の真上に来るように）
    const feetY = p.y - FEET_OFFSET
    rig.position.set(p.x - offX, feetY, p.z - offZ)

    // ---- 落下リスポーン
    if (p.y < respawnY) placeAt(spawnRef.current.position, spawnRef.current.yaw)

    // ---- useUsers().getLocalMovement() 用
    if (movementRef) {
      const m = movementRef.current
      m.position.x = p.x
      m.position.y = feetY
      m.position.z = p.z
      const hs = Math.hypot(vx, vz)
      m.direction.x = hs > 0 ? vx / hs : 0
      m.direction.z = hs > 0 ? vz / hs : 0
      m.horizontalSpeed = hs
      m.verticalSpeed = vy
      m.rotation.yaw = -Math.atan2(_fwd.x, -_fwd.z)
      m.rotation.pitch = 0
      m.isGrounded = Math.abs(v.y) < 0.5
      m.isJumping = vy > 0.5
    }
  })

  return h(
    'group',
    null,
    h('group', { ref: rigRef, name: 'xr-rig' }),
    h(
      RigidBody,
      {
        ref: bodyRef,
        type: 'dynamic',
        position: [spawnPosition[0], spawnPosition[1] + FEET_OFFSET + 0.05, spawnPosition[2]],
        lockRotations: true,
        enabledRotations: [false, false, false],
        friction: 0,
        restitution: 0,
        linearDamping: 0.2,
        canSleep: false,
        colliders: false,
        name: 'local-player',
      },
      h(CapsuleCollider, { args: [PLAYER_HALF_HEIGHT, PLAYER_RADIUS] }),
    ),
  )
}
