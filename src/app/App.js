// XRift ワールドを載せるホスト（xrift-frontend の役割を最小限で担う）
import { Component, Suspense, createElement as h, useMemo } from 'react'
import { useThree } from '@react-three/fiber'
import { Physics } from '@react-three/rapier'
import { XRiftProvider } from '@xrift/world-components'
import { getWorld } from '../xrift/api.js'
import { VRPlayer } from './VRPlayer.js'
import { Controllers } from './Controllers.js'
import { MaterialCompat } from './MaterialCompat.js'

class WorldErrorBoundary extends Component {
  constructor(props) {
    super(props)
    this.state = { error: null }
  }
  static getDerivedStateFromError(error) {
    return { error }
  }
  componentDidCatch(error) {
    console.error('[world] render error:', error)
  }
  render() {
    if (this.state.error) return null
    return this.props.children
  }
}

/** R3F のストアをホスト側から見えるようにする（デバッグ / スクリーンショット用） */
function StoreBridge({ stateRef }) {
  const get = useThree((s) => s.get)
  if (stateRef) stateRef.current = get
  return null
}

const LOCAL_USER = { id: 'local-user', displayName: 'VR Guest', avatarUrl: null, isGuest: true }

export function App({ World, world, baseUrl, movementRef, teleportRef, rigRef, stateRef }) {
  const gravity = world.physics?.gravity ?? 9.81

  const usersImplementation = useMemo(
    () => ({
      localUser: LOCAL_USER,
      remoteUsers: [],
      getMovement: () => undefined,
      getLocalMovement: () => movementRef.current,
      getAvatarHeight: () => undefined,
      getLocalAvatarHeight: () => ({ height: 1.6, eyeHeight: 1.5 }),
    }),
    [movementRef],
  )
  const teleportImplementation = useMemo(() => ({ teleport: (dest) => teleportRef.current?.(dest) }), [teleportRef])
  // useWorld().getWorldInfo() は公開 API から引く
  const worldImplementation = useMemo(
    () => ({
      getWorldInfo: async (worldId) => {
        const w = worldId && worldId !== world.id ? await getWorld(worldId) : world
        return {
          id: w.id,
          name: w.name,
          description: w.description ?? null,
          thumbnailUrl: w.thumbnailUrl ?? null,
          isPublic: w.isPublic ?? true,
          instanceCount: w.instanceCount ?? 0,
          totalVisitCount: w.totalVisitCount ?? 0,
          uniqueVisitorCount: w.uniqueVisitorCount ?? 0,
          favoriteCount: w.favoriteCount ?? 0,
        }
      },
    }),
    [world],
  )

  return h(
    XRiftProvider,
    { baseUrl, usersImplementation, teleportImplementation, worldImplementation },
    h(StoreBridge, { stateRef }),
    h(MaterialCompat, null),
    h(
      Physics,
      { gravity: [0, -gravity, 0], timeStep: 'vary' },
      h(VRPlayer, { movementRef, teleportRef, rigRef, spawnPosition: [0, 0.5, 0] }),
      h(Controllers, { rigRef }),
      h(WorldErrorBoundary, null, h(Suspense, { fallback: null }, h(World))),
    ),
  )
}
