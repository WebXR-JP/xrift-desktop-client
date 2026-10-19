// ネイティブアドオン（OpenXR）とランチャーをビルドし、所定の場所に配置する
import { execFileSync } from 'node:child_process'
import { copyFileSync, existsSync, mkdirSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const root = join(dirname(fileURLToPath(import.meta.url)), '..')
const nativeDir = join(root, 'native')
const cmakeJs = join(root, 'node_modules', 'cmake-js', 'bin', 'cmake-js')

execFileSync(process.execPath, [cmakeJs, 'compile', '--directory', nativeDir, '--config', 'Release', '--arch', 'x64', '-G', 'Visual Studio 17 2022', '-A', 'x64'], {
  stdio: 'inherit',
  cwd: root,
})

const out = join(nativeDir, 'build', 'Release')
const binDir = join(root, 'bin')
mkdirSync(binDir, { recursive: true })
for (const f of ['xrift_openxr.node', 'openxr_loader.dll', 'openvr_api.dll']) {
  copyFileSync(join(out, f), join(binDir, f))
}
// ランチャーはプロジェクトルートに置く（src/main.js を自分の隣から探すため）
copyFileSync(join(out, 'xrift_launcher.exe'), join(root, 'XRiftVR.exe'))

if (!existsSync(join(binDir, 'xrift_openxr.node'))) throw new Error('build output missing')
console.log('\n[build-native] OK -> bin/xrift_openxr.node, XRiftVR.exe')
