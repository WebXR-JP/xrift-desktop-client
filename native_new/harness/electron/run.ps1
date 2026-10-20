#Requires -Version 5.1
# M1 ハーネスの実行（再現手順）。
#  - transfer-addon をビルドし、xrift_new.exe をビルドし、electron で main.js を起動する
#  - electron は XRift-Client/node_modules のものを使う
# 使い方: .\run.ps1 [-Seconds 60] [-Width 4096] [-Height 2048]
param(
  [int]$Seconds = 60,
  [int]$Width = 4096,
  [int]$Height = 2048
)
$ErrorActionPreference = 'Stop'
$harness = $PSScriptRoot                    # native_new/harness/electron
$root = Split-Path -Parent (Split-Path -Parent $harness)  # native_new
$repo = Split-Path -Parent $root            # XRift-Client
$electron = Join-Path $repo 'node_modules\electron\dist\electron.exe'

Write-Host "== 1/4 transfer-addon のビルド =="
& (Join-Path $root 'transfer-addon\build.ps1')

Write-Host "== 2/4 xrift_new.exe のビルド =="
Push-Location $root
cmake --build --preset windows-vs-release
$buildCode = $LASTEXITCODE
Pop-Location
if ($buildCode -ne 0) { throw "exe のビルドに失敗" }

Write-Host "== 3/4 プロトコル配置試験 =="
node (Join-Path $harness 'protocol-test.js')
if ($LASTEXITCODE -ne 0) { throw "protocol-test に失敗" }

Write-Host "== 4/4 electron ハーネスの実行（${Seconds} 秒） =="
$exe = Join-Path $root 'build\windows\Release\xrift_new.exe'
# 出力の取り回しは Start-Process の -RedirectStandardOutput を使う。
# シェル直結びの `>` リダイレクトでは electron が無出力で即終了する環境がある。
$proc = Start-Process -FilePath $electron `
  -ArgumentList @('.', "--exe=$exe", "--seconds=$Seconds", "--width=$Width", "--height=$Height") `
  -WorkingDirectory $harness `
  -RedirectStandardOutput (Join-Path $harness 'run-out.txt') `
  -RedirectStandardError (Join-Path $harness 'run-err.txt') -PassThru
$proc | Wait-Process
$code = $proc.ExitCode
Write-Host "electron 終了 code=$code"
Write-Host "== harness-stats.json =="
Get-Content (Join-Path $harness 'harness-stats.json')
exit $code
