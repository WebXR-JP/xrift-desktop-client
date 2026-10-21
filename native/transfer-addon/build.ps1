#Requires -Version 5.1
# M1 転送アドオンのビルド（再現コマンド）。
# 前提: XRift-Client/node_modules に cmake-js と electron があること。
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot          # native
$repo = Split-Path -Parent $root                    # XRift-Client
$cmakeJs = Join-Path $repo 'node_modules\cmake-js\bin\cmake-js'

node $cmakeJs compile `
  --directory (Join-Path $root 'transfer-addon') --config Release --arch x64 `
  -G 'Visual Studio 17 2022' -A x64 `
  --runtime electron --runtime-version 44.9.0
