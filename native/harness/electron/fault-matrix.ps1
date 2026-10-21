#Requires -Version 5.1
# M3（滞留制限・借用管理・正常終了/障害時停止）の自動マトリクス（検証用）。
# 各ケースを順に実行し、期待（DESIGN §6 に基づく判定基準）と実測を突き合わせ、
# 違反があれば非ゼロ終了する。製品経路の挙動は変えない（注入は既定 off）。
# 使い方: .\fault-matrix.ps1 [-Seconds 15] [-Width 2048] [-Height 1024] [-Only normal,delay]
# 結果は fault-matrix-results.md と fault-matrix-results.csv に保存する。
param(
  [int]$Seconds = 15,
  [int]$Width = 2048,
  [int]$Height = 1024,
  [int]$CaseTimeoutS = 60,
  [string]$OutDir = '',
  [string[]]$Only = @()
)
$ErrorActionPreference = 'Stop'
$harness = $PSScriptRoot                    # native/harness/electron
$root = Split-Path -Parent (Split-Path -Parent $harness)  # native

if ($Seconds -lt 5 -or $Seconds -gt 20) {
  throw "Seconds は 5〜20 の範囲であること（指定=$Seconds）"
}

# 期待値の表（DESIGN §6 に基づく判定基準。これを合否の正本にする）。
# 変える場合は根拠を報告すること。
$Cases = @(
  @{ Name = 'normal'; Desc = '正常';
     Run = @{ };
     Expect = '正常: harness 0 / exe 0、quarantine 0、offers=dones、maxHeld≤2' },
  @{ Name = 'exe-kill'; Desc = 'exe を外部 kill（約5秒後）';
     Run = @{ }; KillExeAfterS = 5;
     Expect = 'harness 非ゼロ、quarantine≥1、release されない、ハングなし' },
  @{ Name = 'electron-kill'; Desc = 'electron を外部 kill（約5秒後）';
     Run = @{ }; KillElectronAfterS = 5;
     Expect = 'exe が 10 秒以内に終了、ハングなし' },
  @{ Name = 'drop-done'; Desc = 'ACK 消失の模擬';
     Run = @{ DropDoneAt = 30 };
     Expect = 'harness 非ゼロ、quarantine≥1、maxHeld≤2' },
  @{ Name = 'unconfirmed'; Desc = '完了未確認の模擬';
     Run = @{ InjectUnconfirmed = 30 };
     Expect = 'exe 非ゼロ、Done を送っていない（offers−dones=1）、harness 非ゼロ、quarantine 1' },
  @{ Name = 'release-fail'; Desc = '解放失敗の模擬';
     # N=1000 は定常到達後を狙う（release 試行は paint+Done で約 175/s のため約 6 秒後。
     # 小さすぎると起動過渡（Offer 前）に当たり、定常の借用隔離を試せない）。
     Run = @{ InjectReleaseFail = 1000 };
     Expect = 'harness 非ゼロ、quarantine≥1、二重 release なし' },
  @{ Name = 'renderer-crash'; Desc = 'renderer 強制クラッシュ（5秒後）';
     Run = @{ InjectRendererCrashMs = 5000 };
     Expect = 'ハングなし、非ゼロ、未 ACK があれば隔離' },
  @{ Name = 'gpu-kill'; Desc = 'GPU プロセス kill（5秒後）';
     Run = @{ InjectGpuKillMs = 5000 };
     Expect = 'ハングなし＋taskkill 0＋対象 pid 消滅（落下の証拠）。再起動の有無は記録し、終了コードは参考値' },
  @{ Name = 'delay'; Desc = 'consumer 遅延 50ms';
     Run = @{ DelayMs = 50 };
     Expect = 'harness 0 / exe 0、maxHeld≤2、drops 増、ハンドル数が増え続けない' }
)

# ケース開始以降に起動した検証プロセスだけを対象にする（同名の別作業を殺さない）。
# electron は Path が検証用（XRift-Client 配下）のものに限る（別物の Electron を殺さない）。
function Get-TestProcesses {
  param([string]$ProcName, [datetime]$Since)
  $found = @()
  foreach ($p in (Get-Process -Name $ProcName -ErrorAction SilentlyContinue)) {
    try {
      if ($p.StartTime -lt $Since) { continue }
      if ($ProcName -eq 'electron') {
        $pp = ''
        try { $pp = $p.Path } catch { $pp = '' }
        if ($pp -notlike '*XRift-Client*') { continue }
      }
      $found += $p
    } catch {
      # 開始時刻が取れないプロセスは対象外にする（誤殺を避ける）
    }
  }
  return $found
}

function Kill-TestProcesses {
  param([string]$ProcName, [datetime]$Since)
  foreach ($p in (Get-TestProcesses -ProcName $ProcName -Since $Since)) {
    try { Stop-Process -InputObject $p -Force -ErrorAction SilentlyContinue } catch { }
  }
}

function Test-TestProcessAlive {
  param([string]$ProcName, [datetime]$Since)
  return ((Get-TestProcesses -ProcName $ProcName -Since $Since).Count -gt 0)
}

# harness-run.log の末尾にある exe 終了行から終了コードを取る。
# 形式: exe exit: code=<code> signal=<signal>（kill 死は code が数値でない）。
function Get-ExeExit {
  param([string]$LogPath)
  if (-not (Test-Path $LogPath)) { return $null }
  $hits = Select-String -Path $LogPath -Pattern 'exe exit: code=(\S+) signal=(\S*)' -AllMatches -ErrorAction SilentlyContinue
  if (-not $hits) { return $null }
  $last = $hits[-1].Matches[0].Groups
  $codeText = $last[1].Value
  $code = $null
  # 外部 kill 死は code が 32bit 範囲外（例: 4294967295）や null になる。
  # 0 か否かだけが判定に要るため long で受ける。
  if ($codeText -match '^\d+$') {
    try { $code = [long]$codeText } catch { $code = -1 }
  }
  return @{ Code = $code; Signal = $last[2].Value; Raw = $hits[-1].Line }
}

# exe の標準出力（run-out.txt）の末尾にあるハンドル数統計を取る。
# 形式: 転送統計: ハンドル数 初回=<a> 最終=<b> 最大=<c>（増加し続けないこと）
function Get-ExeHandles {
  param([string]$OutPath)
  if (-not (Test-Path $OutPath)) { return $null }
  $hits = Select-String -Path $OutPath -Pattern 'ハンドル数 初回=(\d+) 最終=(\d+) 最大=(\d+)' -AllMatches -ErrorAction SilentlyContinue
  if (-not $hits) { return $null }
  $g = $hits[-1].Matches[0].Groups
  return @{ First = [int]$g[1].Value; Last = [int]$g[2].Value; Max = [int]$g[3].Value }
}

function Read-CaseStats {
  param([string]$StatsPath, [datetime]$Since)
  if (-not (Test-Path $StatsPath)) { return $null }
  try {
    $item = Get-Item $StatsPath
    if ($item.LastWriteTime -lt $Since) { return $null }  # 前ケースの残骸は証拠にしない
    return (Get-Content $StatsPath -Raw | ConvertFrom-Json)
  } catch {
    return $null
  }
}

function Test-LogContains {
  param([string]$LogPath, [string]$Pattern)
  if (-not (Test-Path $LogPath)) { return $false }
  return ($null -ne (Select-String -Path $LogPath -Pattern $Pattern -SimpleMatch -ErrorAction SilentlyContinue | Select-Object -First 1))
}

function Get-LogLineCount {
  param([string]$LogPath, [string]$Pattern)
  if (-not (Test-Path $LogPath)) { return 0 }
  $found = Select-String -Path $LogPath -Pattern $Pattern -AllMatches -ErrorAction SilentlyContinue
  if (-not $found) { return 0 }
  return $found.Count
}

$resultsDir = Join-Path $harness 'logs\fault-matrix'
if (-not (Test-Path $resultsDir)) { New-Item -ItemType Directory $resultsDir | Out-Null }

$onlyNames = @()
foreach ($o in $Only) {
  # '-Only normal,delay' のようなカンマ区切りも受け付ける（呼び出し側の展開差異の吸収）。
  foreach ($s in ("$o" -split ',')) {
    $s = $s.Trim()
    if ($s -ne '') { $onlyNames += $s }
  }
}
$selected = @($Cases | Where-Object { $onlyNames.Count -eq 0 -or $onlyNames -contains $_['Name'] })
if ($selected.Count -eq 0) { throw "実行対象のケースが無い（-Only の指定を確認）" }

$results = @()
$buildDone = $false
$caseIndex = 0
foreach ($case in $selected) {
  $caseIndex++
  $name = $case['Name']
  $caseDir = Join-Path $resultsDir $name
  if (-not (Test-Path $caseDir)) { New-Item -ItemType Directory $caseDir | Out-Null }
  Write-Host ""
  Write-Host "== [$caseIndex/$($selected.Count)] $name : $($case['Desc']) =="

  # 前ケースの残骸を証拠にしないため、共有の出力物を消してから起動する。
  foreach ($f in @('harness-stats.json', 'harness-run.log', 'run-out.txt', 'run-err.txt')) {
    $p = Join-Path $harness $f
    if (Test-Path $p) { Remove-Item $p -Force -ErrorAction SilentlyContinue }
  }
  $caseStart = Get-Date

  # run.ps1 の引数組み立て（run.ps1 と同じ引数体系。二重実装しない）。
  $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $harness 'run.ps1'),
    '-Seconds', "$Seconds", '-Width', "$Width", '-Height', "$Height")
  foreach ($k in $case['Run'].Keys) {
    $argList += "-$k"
    $argList += "$($case['Run'][$k])"
  }
  if ($buildDone) {
    $argList += '-SkipBuild'  # 初回に 1 回だけ建てる（合計 5 分以内に収めるため）。
  }
  Write-Host "run.ps1 $($argList -join ' ')"
  # 進行のトレース（ハング時の切り分け用。即時フラッシュする）。
  $tracePath = Join-Path $caseDir 'matrix-trace.log'
  function Write-Trace {
    param([string]$Msg)
    $line = "$((Get-Date).ToString('o')) $Msg"
    try { Add-Content -Path $tracePath -Value $line -Encoding utf8 } catch { }
    Write-Host "  [trace] $Msg"
  }
  Write-Trace "case start (timeout=${CaseTimeoutS}s)"

  $runOut = Join-Path $caseDir 'run-ps1-out.txt'
  $runErr = Join-Path $caseDir 'run-ps1-err.txt'
  # 出力の取り回しは Start-Process の -RedirectStandardOutput を使う。
  # シェル直結びの `>` リダイレクトでは electron が無出力で即終了する環境がある。
  $proc = Start-Process -FilePath 'powershell.exe' `
    -ArgumentList $argList `
    -WorkingDirectory $harness `
    -RedirectStandardOutput $runOut `
    -RedirectStandardError $runErr -PassThru

  $hang = $false
  $didKillExe = $false
  $didKillElectron = $false
  $electronKillAt = $null
  $exeGoneAfterElectronKillS = $null
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $lastBeat = 0
  while (-not $proc.HasExited) {
    Start-Sleep -Milliseconds 500
    $el = $sw.Elapsed.TotalSeconds
    if ($el - $lastBeat -ge 10) {
      $lastBeat = $el
      Write-Trace "waiting run.ps1 (elapsed=$([math]::Round($el, 1))s)"
    }
    if ($case['KillExeAfterS'] -gt 0 -and -not $didKillExe -and $el -ge $case['KillExeAfterS']) {
      Write-Host "  [注入] xrift_new.exe を外部 kill する"
      Kill-TestProcesses -ProcName 'xrift_new' -Since $caseStart
      $didKillExe = $true
    }
    if ($case['KillElectronAfterS'] -gt 0 -and -not $didKillElectron -and $el -ge $case['KillElectronAfterS']) {
      Write-Host "  [注入] electron を外部 kill する"
      Kill-TestProcesses -ProcName 'electron' -Since $caseStart
      $didKillElectron = $true
      $electronKillAt = Get-Date
    }
    if ($didKillElectron -and $exeGoneAfterElectronKillS -eq $null) {
      if (-not (Test-TestProcessAlive -ProcName 'xrift_new' -Since $caseStart)) {
        $exeGoneAfterElectronKillS = ((Get-Date) - $electronKillAt).TotalSeconds
      }
    }
    if ($el -ge $CaseTimeoutS) {
      Write-Host "  [TIMEOUT] ${CaseTimeoutS} 秒超過。検証プロセスを kill してハング扱いにする"
      $hang = $true
      Kill-TestProcesses -ProcName 'electron' -Since $caseStart
      Kill-TestProcesses -ProcName 'xrift_new' -Since $caseStart
      try {
        if (-not $proc.HasExited) { Stop-Process -InputObject $proc -Force -ErrorAction SilentlyContinue }
      } catch { }
      break
    }
  }
  $sw.Stop()
  $buildDone = $true
  $elapsedS = [math]::Round($sw.Elapsed.TotalSeconds, 1)
  Write-Trace "run.ps1 exited (elapsed=${elapsedS}s)"
  # run.ps1 の終了コード（= harness の終了コード）が取れない場合は原因を残す。
  # HasExited 直後の ExitCode 取得が稀に失敗するため、Refresh と再試行を付ける。
  $harnessExit = $null
  $exitNote = ''
  Write-Trace "reading exit code (proc=$($proc -ne $null) hasExited=$($proc.HasExited))"
  try {
    $proc.Refresh()
    Write-Trace "proc refreshed"
    if ($proc.HasExited) {
      # 終了コードの取得。-RedirectStandardOutput 付きで powershell を子にした場合は
      # $proc.ExitCode が $null になる（5.1 の癖。probe-exit4.ps1 で再現確認済み）。
      # そのため run.ps1 が必ず標準出力に出す「electron 終了 code=X」を正本にする。
      # パターンは ASCII だけにする（5.1 の Select-String は出力リダイレクト由来の
      # ファイルで日本語の一致に失敗することがある。probe-sel.ps1 で確認済み）。
      try { $proc | Wait-Process } catch { }
      try {
        $harnessExit = $proc.ExitCode
        Write-Trace "exit code read: $($harnessExit -eq $null)"
      } catch {
        Write-Trace "exit code read threw: $($_.Exception.Message)"
      }
      if ($harnessExit -eq $null) {
        $fallback = Select-String -Path $runOut -Pattern 'electron.*code=(\d+)' -AllMatches -ErrorAction SilentlyContinue
        if ($fallback) {
          $harnessExit = [int]$fallback[-1].Matches[0].Groups[1].Value
          Write-Trace "exit code fallback from run.ps1 output: $harnessExit"
        }
      }
      if ($harnessExit -eq $null) {
        $harnessExit = 'exitcode-unreadable'
        $exitNote = "run.ps1 プロセス(id=$($proc.Id))の ExitCode が読めなかった"
      }
    } else {
      $harnessExit = 'killed-by-matrix'
    }
  } catch {
    $harnessExit = 'unknown'
    $exitNote = "終了コード取得に失敗: $($_.Exception.Message)"
  }
  Write-Trace "exit code done: $harnessExit"

  # electron-kill 直後の exe 終了待ち（最大 10 秒）。切断で終了する想定である。
  if ($didKillElectron -and $exeGoneAfterElectronKillS -eq $null) {
    $waitSw = [Diagnostics.Stopwatch]::StartNew()
    while ($waitSw.Elapsed.TotalSeconds -lt 10) {
      if (-not (Test-TestProcessAlive -ProcName 'xrift_new' -Since $caseStart)) {
        $exeGoneAfterElectronKillS = ((Get-Date) - $electronKillAt).TotalSeconds
        break
      }
      Start-Sleep -Milliseconds 500
    }
    $waitSw.Stop()
    if ($exeGoneAfterElectronKillS -eq $null) {
      Kill-TestProcesses -ProcName 'xrift_new' -Since $caseStart
    }
  }

  # 共有の出力物をケース別に退避する（次ケースが上書きするため）。
  $statsPath = Join-Path $harness 'harness-stats.json'
  $logPath = Join-Path $harness 'harness-run.log'
  $exeOutPath = Join-Path $harness 'run-out.txt'
  foreach ($pair in @(($statsPath, 'harness-stats.json'), ($logPath, 'harness-run.log'), ($exeOutPath, 'run-out.txt'))) {
    if (Test-Path $pair[0]) { Copy-Item $pair[0] (Join-Path $caseDir $pair[1]) -Force -ErrorAction SilentlyContinue }
  }
  Write-Trace "evidence copied"

  $stats = Read-CaseStats -StatsPath (Join-Path $caseDir 'harness-stats.json') -Since $caseStart
  Write-Trace "stats read (offers=$($stats.offers))"
  $exeExit = Get-ExeExit -LogPath (Join-Path $caseDir 'harness-run.log')
  Write-Trace "exe exit parsed"
  $exeHandles = Get-ExeHandles -OutPath (Join-Path $caseDir 'run-out.txt')
  Write-Trace "exe handles parsed"
  $offers = $null; $dones = $null; $quarantine = $null; $maxHeld = $null; $drops = $null
  if ($stats -ne $null) {
    $offers = [int]$stats.offers; $dones = [int]$stats.dones
    $quarantine = [int]$stats.quarantined; $maxHeld = [int]$stats.maxHeld; $drops = [int]$stats.drops
  }

  # 期待との突き合わせ（判定基準は冒頭の $Cases の Expect が正本）。
  $fails = @()
  $notes = @()
  if ($exitNote -ne '') { $notes += $exitNote }
  # harness 終了コードが数値で取れたか（取れない kill 死等は別扱いにする）。
  $harnessNumeric = ($harnessExit -is [int])
  switch ($name) {
    'normal' {
      if ($hang) { $fails += 'ハングした' }
      if ($stats -eq $null) { $fails += '統計が無い' }
      else {
        if ($harnessExit -ne 0) { $fails += "harness 終了=${harnessExit}（期待 0）" }
        if ($exeExit -eq $null -or $exeExit['Code'] -ne 0) { $fails += "exe 終了=$($exeExit | Out-String)（期待 0）" }
        if ($quarantine -ne 0) { $fails += "quarantine=${quarantine}（期待 0）" }
        if ($offers -ne $dones) { $fails += "offers=${offers} dones=${dones}（期待 offers=dones）" }
        if ($offers -le 0) { $fails += "offers=${offers}（期待 >0。受信自体が無い）" }
        if ($maxHeld -gt 2) { $fails += "maxHeld=${maxHeld}（期待 ≤2）" }
      }
    }
    'exe-kill' {
      if ($hang) { $fails += 'ハングした' }
      if (-not $didKillExe) { $fails += 'kill 注入が実行されなかった' }
      if ($stats -eq $null) { $fails += '統計が無い' }
      else {
        if ($harnessNumeric -and $harnessExit -eq 0) { $fails += "harness 終了=${harnessExit}（期待 非ゼロ）" }
        if ($quarantine -lt 1) { $fails += "quarantine=${quarantine}（期待 ≥1。未 ACK を release せず隔離）" }
      }
      if (Test-TestProcessAlive -ProcName 'xrift_new' -Since $caseStart) { $fails += 'exe が残存している' }
    }
    'electron-kill' {
      if ($hang) { $fails += 'ハングした' }
      if (-not $didKillElectron) { $fails += 'kill 注入が実行されなかった' }
      if ($exeGoneAfterElectronKillS -eq $null) {
        $fails += 'exe が electron kill の 10 秒以内に終了しなかった'
      } else {
        $notes += "exe は electron kill の $([math]::Round($exeGoneAfterElectronKillS, 1)) 秒後に終了"
      }
      $notes += "harness 終了=${harnessExit}（kill 死のため参考値）"
    }
    'drop-done' {
      if ($hang) { $fails += 'ハングした' }
      if ($stats -eq $null) { $fails += '統計が無い' }
      else {
        if ($harnessNumeric -and $harnessExit -eq 0) { $fails += "harness 終了=${harnessExit}（期待 非ゼロ）" }
        if ($quarantine -lt 1) { $fails += "quarantine=${quarantine}（期待 ≥1）" }
        if ($maxHeld -gt 2) { $fails += "maxHeld=${maxHeld}（期待 ≤2）" }
        if ([int]$stats.droppedDone -lt 1) { $fails += "droppedDone=$($stats.droppedDone)（期待 ≥1。注入が当たっていない）" }
      }
    }
    'unconfirmed' {
      if ($hang) { $fails += 'ハングした' }
      if ($stats -eq $null) { $fails += '統計が無い' }
      else {
        if ($exeExit -eq $null -or $exeExit['Code'] -eq 0) { $fails += "exe 終了=$($exeExit | Out-String)（期待 非ゼロ）" }
        elseif ($exeExit['Code'] -ne 2) { $notes += "exe 終了=$($exeExit['Code'])（2 以外の非ゼロ。経路を確認）" }
        if ($harnessNumeric -and $harnessExit -eq 0) { $fails += "harness 終了=${harnessExit}（期待 非ゼロ）" }
        if (($offers - $dones) -ne 1) { $fails += "offers−dones=$($offers - $dones)（期待 1。Done を送っていない）" }
        if ($quarantine -ne 1) { $fails += "quarantine=${quarantine}（期待 1）" }
      }
    }
    'release-fail' {
      if ($hang) { $fails += 'ハングした' }
      if ($stats -eq $null) { $fails += '統計が無い' }
      else {
        if ($harnessNumeric -and $harnessExit -eq 0) { $fails += "harness 終了=${harnessExit}（期待 非ゼロ）" }
        if ($quarantine -lt 1) { $fails += "quarantine=${quarantine}（期待 ≥1）" }
      }
      $failLines = Get-LogLineCount -LogPath (Join-Path $caseDir 'harness-run.log') -Pattern 'release FAILED borrow='
      if ($failLines -ne 1) { $fails += "release 失敗行=${failLines} 件（期待 1。二重 release の疑い）" }
      if (Test-LogContains -LogPath (Join-Path $caseDir 'harness-run.log') -Pattern '保留が二重') {
        $fails += '保留の二重登録がある（二重 release の疑い）'
      }
    }
    'renderer-crash' {
      if ($hang) { $fails += 'ハングした' }
      if (-not (Test-LogContains -LogPath (Join-Path $caseDir 'harness-run.log') -Pattern 'render-process-gone')) {
        $fails += 'render-process-gone の検知が無い（注入が当たっていない）'
      }
      if ($stats -eq $null) { $fails += '統計が無い' }
      else {
        if ($harnessNumeric -and $harnessExit -eq 0) { $fails += "harness 終了=${harnessExit}（期待 非ゼロ）" }
        if (($offers - $dones) -gt 0 -and $quarantine -lt 1) {
          $fails += "未 ACK あり（offers−dones=$($offers - $dones)）だが quarantine=${quarantine}（期待 ≥1）"
        }
        if (($offers - $dones) -eq 0) { $notes += 'クラッシュ時に未 ACK が無かったため隔離 0 を許容' }
      }
      $notes += 'exe は切断で終了する想定（exe 終了は参考値）'
    }
    'gpu-kill' {
      # 前提条件は GPU プロセスが実際に落ちたこと。直接証拠は taskkill 終了 0 と
      # OS 上での対象 pid の消滅。kill 後・終了時の getAppMetrics の gpu pid は
      # 再起動の有無の状況証拠（単発の一致では生死を判断しない。stale を返す場合がある）。
      # 落ちていなければ注入不成立として FAIL にする（PASS 扱いにしない）。
      # 終了コードは回復の有無で合否を分けず記録のみ（回復すれば 0 も許容）。
      if ($hang) { $fails += 'ハングした' }
      if (-not (Test-LogContains -LogPath (Join-Path $caseDir 'harness-run.log') -Pattern 'taskkill /F /PID')) {
        $fails += 'GPU kill（taskkill）の実行記録が無い（注入が当たっていない）'
      }
      if ($stats -eq $null) { $fails += '統計が無い' }
      else {
        if ($stats.gpuKillRequested -ne 1) { $fails += 'gpuKillRequested が 1 でない（kill 未実行）' }
        $notes += "kill 前 pid=$($stats.gpuPidStart) kill 対象=$($stats.gpuKilledPid) 手段=$($stats.gpuKillMethod) taskkill終了=$($stats.gpuKillExitCode) pid消滅(直後)=$($stats.gpuKillPidGone) pid消滅(5秒後)=$($stats.gpuKillPidGoneAfterWait) kill後観測=$($stats.gpuPidAfterKill) 終了時=$($stats.gpuPid) 描画(kill時→終了時)=$($stats.gpuPaintsAtKill)→$($stats.paints)"
        if ($stats.gpuKillExitCode -ne 0) {
          $fails += "taskkill が失敗 code=$($stats.gpuKillExitCode)（注入不成立）"
        } elseif ($stats.gpuKillPidGone -ne 1) {
          $fails += "kill 後に pid が OS 上に残存（落下せず）。注入不成立"
        } else {
          $notes += 'GPU プロセスの落下を確認（taskkill 0＋OS 上で pid 消滅）'
          if ($stats.gpuKillPidGoneAfterWait -eq 1) { $notes += '5秒後も OS 上に pid なし（落下が確定）' }
          if ("$($stats.gpuPidAfterKill)" -ne "$($stats.gpuKilledPid)" -or "$($stats.gpuPid)" -ne "$($stats.gpuKilledPid)") {
            $notes += 'kill 後・終了時の gpu pid が kill 前と異なる（再起動または GPU なし継続。ログを確認）'
          } else {
            $notes += '注意: 観測 pid が kill 前と同一（getAppMetrics の stale の可能性。OS 消滅を主証拠にする）'
          }
        }
      }
      if ($harnessNumeric -and $harnessExit -eq 0) {
        $notes += 'RECOVERED: harness は 0 終了（kill 後も継続できた。ログを確認）'
      } else {
        $notes += "harness 終了=${harnessExit}（回復せず停止。非ゼロが望ましい側）"
      }
    }
    'delay' {
      if ($hang) { $fails += 'ハングした' }
      if ($stats -eq $null) { $fails += '統計が無い' }
      else {
        if ($harnessExit -ne 0) { $fails += "harness 終了=${harnessExit}（期待 0）" }
        if ($exeExit -eq $null -or $exeExit['Code'] -ne 0) { $fails += "exe 終了=$($exeExit | Out-String)（期待 0）" }
        if ($maxHeld -gt 2) { $fails += "maxHeld=${maxHeld}（期待 ≤2）" }
        if ($drops -le 0) { $fails += "drops=${drops}（期待 >0。遅延で drop が増える）" }
      }
      if ($exeHandles -eq $null) {
        $fails += 'exe のハンドル数統計が無い'
      } else {
        $notes += "exe ハンドル数 初回=$($exeHandles['First']) 最終=$($exeHandles['Last']) 最大=$($exeHandles['Max'])"
        if (($exeHandles['Max'] - $exeHandles['First']) -gt 20) {
          $fails += "ハンドル数が増加傾向（初回=$($exeHandles['First']) 最大=$($exeHandles['Max'])。期待は増え続けない）"
        }
      }
    }
    default { $fails += "未知のケース ${name}" }
  }

  $verdict = 'PASS'
  if ($hang -or $fails.Count -gt 0) { $verdict = 'FAIL' }
  $exeExitText = 'n/a'
  if ($exeExit -ne $null) {
    $exeExitText = if ($exeExit['Code'] -eq $null) { "signal=$($exeExit['Signal'])" } else { "$($exeExit['Code'])" }
  }
  $row = [pscustomobject]@{
    Case = $name
    HarnessExit = if ($harnessExit -eq $null) { 'n/a' } else { "$harnessExit" }
    ExeExit = $exeExitText
    Offers = if ($offers -eq $null) { 'n/a' } else { "$offers" }
    Dones = if ($dones -eq $null) { 'n/a' } else { "$dones" }
    Quarantine = if ($quarantine -eq $null) { 'n/a' } else { "$quarantine" }
    MaxHeld = if ($maxHeld -eq $null) { 'n/a' } else { "$maxHeld" }
    Drops = if ($drops -eq $null) { 'n/a' } else { "$drops" }
    Hang = if ($hang) { 'yes' } else { 'no' }
    ElapsedS = $elapsedS
    Verdict = $verdict
    Note = (($fails + $notes) -join ' / ')
  }
  $results += $row
  Write-Host "  -> harness=${harnessExit} exe=${exeExitText} offers=${offers} dones=${dones} quarantine=${quarantine} maxHeld=${maxHeld} drops=${drops} hang=$($row.Hang) ${elapsedS}s [$verdict]"
  if ($fails.Count -gt 0) { Write-Host "     違反: $($fails -join ' / ')" }
}

Write-Host ""
Write-Host "== 結果 =="
$results | Format-Table -AutoSize | Out-String | Write-Host

$mdOut = if ($OutDir -ne '') { Join-Path $OutDir 'fault-matrix-results.md' } else { Join-Path $harness 'fault-matrix-results.md' }
$csvOut = if ($OutDir -ne '') { Join-Path $OutDir 'fault-matrix-results.csv' } else { Join-Path $harness 'fault-matrix-results.csv' }
$md = @()
$md += '# M3 障害マトリクスの結果'
$md += ''
$md += "実行日時: $((Get-Date).ToString('o')) / 条件: Seconds=${Seconds} ${Width}x${Height} / タイムアウト: ${CaseTimeoutS}s"
$md += ''
$md += '| ケース | harness | exe | offers | dones | quarantine | maxHeld | drops | ハング | 所要秒 | 合否 | 備考 |'
$md += '|---|---|---|---|---|---|---|---|---|---|---|---|'
foreach ($r in $results) {
  $note = ($r.Note -replace '\|', '/')
  $md += "| $($r.Case) | $($r.HarnessExit) | $($r.ExeExit) | $($r.Offers) | $($r.Dones) | $($r.Quarantine) | $($r.MaxHeld) | $($r.Drops) | $($r.Hang) | $($r.ElapsedS) | $($r.Verdict) | $note |"
}
$md += ''
$md += '## 期待値（判定基準。DESIGN §6 に基づく）'
$md += ''
foreach ($c in $selected) {
  $md += "- $($c['Name']): $($c['Expect'])"
}
$md -join "`n" | Out-File -Encoding utf8 $mdOut
$results | Export-Csv -Encoding utf8 -NoTypeInformation $csvOut
Write-Host "Markdown: $mdOut"
Write-Host "CSV: $csvOut"

$failed = @($results | Where-Object { $_.Verdict -ne 'PASS' })
if ($failed.Count -gt 0) {
  Write-Host "違反 $($failed.Count) 件あり（非ゼロ終了）"
  exit 1
}
Write-Host "全ケースが期待どおり（ゼロ終了）"
exit 0
