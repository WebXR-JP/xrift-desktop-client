// XRift の公開 API（ログイン不要の範囲）
const API_BASE = process.env.XRIFT_API_BASE || 'https://api.xrift.net'

async function getJson(path) {
  const res = await fetch(API_BASE + path, { headers: { Accept: 'application/json' } })
  if (!res.ok) throw new Error(`XRift API ${path}: HTTP ${res.status}`)
  return res.json()
}

/** 公開ワールドの一覧（コードワールドのみ。EDITOR 型はバンドルが無いので除く） */
export async function listWorlds({ query } = {}) {
  const { worlds } = await getJson('/api/worlds')
  let list = worlds.filter((w) => w.sceneUrl && w.status === 'ACTIVE')
  if (query) {
    const q = query.toLowerCase()
    list = list.filter((w) => w.name.toLowerCase().includes(q) || (w.description ?? '').toLowerCase().includes(q))
  }
  return list
}

export async function getWorld(id) {
  return getJson(`/api/worlds/${encodeURIComponent(id)}`)
}

/** ワールド ID / XRift の URL / 名前（部分一致）/ remoteEntry.js の URL のどれでも受け付ける */
export async function resolveWorld(input) {
  if (!input) {
    const [first] = await listWorlds()
    if (!first) throw new Error('公開ワールドが見つかりません')
    return first
  }
  if (/remoteEntry\.js(\?|$)/.test(input)) {
    return { id: 'local', name: input, sceneUrl: input, physics: null, camera: null, permissions: null }
  }
  const uuid = input.match(/[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}/i)
  if (uuid) return getWorld(uuid[0])
  const hits = await listWorlds({ query: input })
  if (!hits.length) throw new Error(`ワールド "${input}" が見つかりません（--list で一覧を確認できます）`)
  return hits[0]
}
