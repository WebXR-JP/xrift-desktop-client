import { join } from 'node:path'
import { homedir } from 'node:os'

const base = process.env.XRIFT_CLIENT_HOME || join(process.env.LOCALAPPDATA || join(homedir(), '.cache'), 'xrift-openxr-client')

export function cacheDir(sub = '') {
  return sub ? join(base, 'cache', sub) : join(base, 'cache')
}

export function dataDir() {
  return base
}
