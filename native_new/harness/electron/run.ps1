#Requires -Version 5.1
# M1 ハーネスの実行（再現手順）。
#  - transfer-addon をビルドし、xrift_new.exe をビルドし、electron で main.js を起動する
#  - electron は XRift-Client/node_modules のものを使う
# 使い方: .\run.ps1 [-Seconds 60] [-Width 4096] [-Height 2048]
#   [-InjectBadTile N] [-InjectStaleTile N] [-InjectBadCorner N] [-InjectBadBit N] [-NoGpuCheck]
#   [-FailAfter N] [-InjectUnconfirmed N] [-DelayMs N] [-DropDoneAt N]
#   [-InjectReleaseFail N] [-InjectRendererCrashMs Ms] [-InjectGpuKillMs Ms]
#   [-Mode vulkan|d3d-open-only|no-gpu] [-MemLog <接頭辞>]
#   [-M2] [-InjectStaleRecord N] [-RenderScale <0.25〜1.0>] [-NoSubmitVerify]
#   [-DumpSubmit <dir>] [-DumpFrame N]
#   [-Space auto|local|stage|local-floor] [-HapticAtMs Ms[,Ms...]] [-HapticRepeat N]
#   [-InjectSessionRestartMs Ms] [-InjectSpaceChangeMs Ms]
#   [-Guide] [-NoGuide] [-GuidePlan default|short] [-CaptureAt 'Ms,path[;Ms,path]']
#   [-SkipBuild]
#   -Mode は exe の診断モード（既定 vulkan=Vulkan 経路）。借用台帳・ACK・release は同じ。
#   d3d-open-only は D3D11 Open 経路の診断専用であり、長時間動かすとメモリが増える。
#   -InjectReleaseFail/-InjectRendererCrashMs/-InjectGpuKillMs は M3 の harness 側注入
#   （いずれも既定 0=無効。通常経路の挙動は変えない）。詳細は README の M3 節を参照。
#   -SkipBuild は fault-matrix.ps1 が初回に 1 回だけ建てるための省略用（通常は付けない）。
#   -Space/-HapticAtMs/-HapticRepeat/-InjectSessionRestartMs/-InjectSpaceChangeMs は
#   M4 用（-M2 と併用。詳細は README の M4 節を参照）。
#   -Guide/-NoGuide は HMD 操作ガイドの HTML オーバーレイ（-M2 時は既定 on、それ以外は off）。
#   -GuidePlan は手順プラン（既定 default。short は 40 秒用 13 ステップ。-M2 時のみ有効）。
#   -CaptureAt は検証専用の PNG 保存（例: -CaptureAt '8000,logs\guide.png'。描画経路に入れない）。
#   -MemLog は計測用 CSV の接頭辞（例: logs\run1 → run1.exe.csv と run1.gpu.csv）。
#     exe 側の 1 秒周期ログ（Private Bytes/RSS/ハンドル/生存 Vk 資源）と、
#     GPU プロセスメモリの 1 秒周期ログ（Get-Counter の Local/Dedicated Usage）を出す。
param(
  [int]$Seconds = 60,
  [int]$Width = 4096,
  [int]$Height = 2048,
  [int]$InjectBadTile = 0,
  [int]$InjectStaleTile = 0,
  [int]$InjectBadCorner = 0,
  [int]$InjectBadBit = 0,
  [int]$FailAfter = 0,
  [int]$InjectUnconfirmed = 0,
  [int]$DelayMs = 0,
  [int]$DropDoneAt = 0,
  [int]$InjectReleaseFail = 0,
  [int]$InjectRendererCrashMs = 0,
  [int]$InjectGpuKillMs = 0,
  [ValidateSet('vulkan', 'd3d-open-only', 'no-gpu')]
  [string]$Mode = 'vulkan',
  [string]$MemLog = '',
  [switch]$NoGpuCheck,
  [switch]$M2,
  [int]$InjectStaleRecord = 0,
  [string]$RenderScale = '0.5',
  [switch]$NoSubmitVerify,
  [string]$DumpSubmit = '',
  [int]$DumpFrame = 60,
  [ValidateSet('auto', 'local', 'stage', 'local-floor')]
  [string]$Space = 'auto',
  # カンマ区切りでも配列でも受け付ける（例: -HapticAtMs 5000,15000 / -HapticAtMs '5000,15000'）。
  # PowerShell の `5000,15000` は配列になるため、electron へ渡す前に結合する。
  [string[]]$HapticAtMs = @('0'),
  [int]$HapticRepeat = 0,
  [int]$InjectSessionRestartMs = 0,
  [int]$InjectSpaceChangeMs = 0,
  [switch]$Guide,
  [switch]$NoGuide,
  [ValidateSet('default', 'short')]
  [string]$GuidePlan = 'default',
  [string]$CaptureAt = '',
  [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$harness = $PSScriptRoot                    # native_new/harness/electron
$root = Split-Path -Parent (Split-Path -Parent $harness)  # native_new
$repo = Split-Path -Parent $root            # XRift-Client
$electron = Join-Path $repo 'node_modules\electron\dist\electron.exe'

Write-Host "== 1/4 transfer-addon のビルド =="
if (-not $SkipBuild) {
  & (Join-Path $root 'transfer-addon\build.ps1')
} else {
  Write-Host "(SkipBuild のため省略)"
}

Write-Host "== 2/4 xrift_new.exe のビルド =="
if (-not $SkipBuild) {
  Push-Location $root
  cmake --build --preset windows-vs-release
  $buildCode = $LASTEXITCODE
  Pop-Location
  if ($buildCode -ne 0) { throw "exe のビルドに失敗" }
} else {
  Write-Host "(SkipBuild のため省略)"
}

Write-Host "== 3/4 プロトコル配置試験 =="
node (Join-Path $harness 'protocol-test.js')
if ($LASTEXITCODE -ne 0) { throw "protocol-test に失敗" }
node (Join-Path $harness 'm2-codec-test.js')
if ($LASTEXITCODE -ne 0) { throw "m2-codec-test に失敗" }
if ($M2) {
  Write-Host "== 3b/4 M2 レンダラの画素試験 =="
  & $electron (Join-Path $harness 'm2-render-test.js')
  if ($LASTEXITCODE -ne 0) { throw "m2-render-test に失敗" }
}

Write-Host "== 4/4 electron ハーネスの実行（${Seconds} 秒・mode=${Mode}） =="
$exe = Join-Path $root 'build\windows\Release\xrift_new.exe'
$electronArgs = @('.', "--exe=$exe", "--seconds=$Seconds", "--width=$Width", "--height=$Height",
  "--mode=$Mode",
  "--inject-bad-tile=$InjectBadTile", "--inject-stale-tile=$InjectStaleTile",
  "--inject-bad-corner=$InjectBadCorner", "--inject-bad-bit=$InjectBadBit",
  "--fail-after=$FailAfter", "--inject-unconfirmed=$InjectUnconfirmed",
  "--delay-ms=$DelayMs", "--drop-done-at=$DropDoneAt",
  "--inject-release-fail=$InjectReleaseFail",
  "--inject-renderer-crash=$InjectRendererCrashMs",
  "--inject-gpu-kill=$InjectGpuKillMs")
if ($NoGpuCheck) { $electronArgs += '--no-gpu-check' }
if ($M2) {
  $electronArgs += '--m2'
  $electronArgs += "--inject-stale-record=$InjectStaleRecord"
  $electronArgs += "--render-scale=$RenderScale"
  $electronArgs += "--space=$Space"
  # ガイドは -M2 時は既定 on（-NoGuide で off）。-M2 以外は -Guide のときだけ on。
  if ($NoGuide) { $electronArgs += '--guide=0' }
  elseif ($Guide -or $M2) { $electronArgs += '--guide=1' }
  $electronArgs += "--guide-plan=$GuidePlan"
  if ($CaptureAt -ne '') { $electronArgs += "--capture-at=$CaptureAt" }
  $hapticAtJoined = ($HapticAtMs -join ',')
  if ($hapticAtJoined -ne '0' -and $hapticAtJoined -ne '') {
    $electronArgs += "--haptic-at=$hapticAtJoined"
  }
  if ($HapticRepeat -gt 0) { $electronArgs += "--haptic-repeat=$HapticRepeat" }
  if ($InjectSessionRestartMs -gt 0) { $electronArgs += "--inject-session-restart=$InjectSessionRestartMs" }
  if ($InjectSpaceChangeMs -gt 0) { $electronArgs += "--inject-space-change=$InjectSpaceChangeMs" }
  if ($NoSubmitVerify) { $electronArgs += '--no-submit-verify' }
  if ($DumpSubmit -ne '') {
    $dumpDir = [IO.Path]::GetFullPath($DumpSubmit)
    $electronArgs += "--dump-submit=$dumpDir"
    $electronArgs += "--dump-frame=$DumpFrame"
  }
  Write-Host "M2 モード: --m2 --inject-stale-record=$InjectStaleRecord --render-scale=$RenderScale --space=$Space"
}
else {
  # M2 以外は既定 off。明示の -Guide/-CaptureAt だけ通す（既存検証に影響しない）。
  if ($Guide) { $electronArgs += '--guide=1' }
  if ($NoGuide) { $electronArgs += '--guide=0' }
  if ($CaptureAt -ne '') { $electronArgs += "--capture-at=$CaptureAt" }
}
# 計測用 CSV（-MemLog <接頭辞>）。exe 側は <接頭辞>.exe.csv（--mem-log 経由）、
# GPU プロセスメモリは <接頭辞>.gpu.csv（このスクリプトが 1 秒周期で採取）。
$sampler = $null
if ($MemLog -ne '') {
  # 相対指定は run.ps1 の呼び出し元 cwd 基準で絶対化する（exe の cwd と食い違わせない）。
  $memBase = [IO.Path]::GetFullPath($MemLog)
  $exeCsv = "$memBase.exe.csv"
  $gpuCsv = "$memBase.gpu.csv"
  $electronArgs += "--mem-log=$exeCsv"
  foreach ($p in @($exeCsv, $gpuCsv)) {
    $d = Split-Path -Parent $p
    if ($d -ne '' -and -not (Test-Path $d)) { New-Item -ItemType Directory $d | Out-Null }
  }
  'timestamp,counter,instance,valueBytes' | Out-File -Encoding utf8 $gpuCsv
  # バックグラウンドで GPU Process Memory を 1 秒周期で取る。
  # インスタンス名は pid_<pid>_luid_... 形式のため、harness-stats.json の gpuPid と
  # 突き合わせて該当 GPU プロセスの列を特定する。
  $sampler = Start-Job -ScriptBlock {
    param($out)
    $counters = @('\GPU Process Memory(*)\Local Usage', '\GPU Process Memory(*)\Dedicated Usage')
    while ($true) {
      try {
        $s = Get-Counter -Counter $counters -ErrorAction Stop
        $ts = (Get-Date).ToString('o')
        foreach ($sm in $s.CounterSamples) {
          $val = [int64]$sm.CookedValue
          "$ts,$($sm.Path),$($sm.InstanceName),$val" | Out-File -Append -Encoding utf8 $out
        }
      } catch {
        # カウンタ取得の失敗は採取の欠損として無視する（計測は転送に影響させない）
      }
      Start-Sleep -Seconds 1
    }
  } -ArgumentList $gpuCsv
  Write-Host "GPU メモリ採取開始: $gpuCsv"
}
# 出力の取り回しは Start-Process の -RedirectStandardOutput を使う。
# シェル直結びの `>` リダイレクトでは electron が無出力で即終了する環境がある。
$proc = Start-Process -FilePath $electron `
  -ArgumentList $electronArgs `
  -WorkingDirectory $harness `
  -RedirectStandardOutput (Join-Path $harness 'run-out.txt') `
  -RedirectStandardError (Join-Path $harness 'run-err.txt') -PassThru
$proc | Wait-Process
$code = $proc.ExitCode
if ($sampler -ne $null) {
  Stop-Job $sampler | Out-Null
  Remove-Job $sampler -Force | Out-Null
  Write-Host "GPU メモリ採取終了"
}
Write-Host "electron 終了 code=$code"
Write-Host "== harness-stats.json =="
Get-Content (Join-Path $harness 'harness-stats.json')
exit $code
